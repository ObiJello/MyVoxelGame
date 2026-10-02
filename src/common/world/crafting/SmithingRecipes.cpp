// File: src/common/world/crafting/SmithingRecipes.cpp
#include "common/world/crafting/SmithingRecipes.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/tags/DataTags.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace Game::SmithingRecipes {

    namespace {

        // An Ingredient: item ids and "#tag" entries (HolderSet<Item>);
        // `present` false is Optional.empty() — the square must be empty.
        struct Ingredient {
            bool                     present = false;
            std::vector<std::string> entries;

            bool Test(const ItemStack& stack) const {
                if (!present) return stack.IsEmpty();
                if (stack.IsEmpty()) return false;
                const std::string_view slug = ItemRegistry::Slug(stack.itemId);
                for (const std::string& e : entries) {
                    if (!e.empty() && e[0] == '#') {
                        if (DataTags::HasTag(DataTags::Registry::Item, slug, e)) return true;
                    } else {
                        std::string_view id = e;
                        if (id.rfind("minecraft:", 0) == 0) id.remove_prefix(10);
                        if (id == slug) return true;
                    }
                }
                return false;
            }
        };

        struct Recipe {
            enum class Kind { Transform, Trim } kind = Kind::Transform;
            std::string id;
            Ingredient  templ, base, addition;
            ItemID      resultItem = Items::Air;   // Transform
            int         resultCount = 1;
            std::string pattern;                    // Trim
        };

        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        Ingredient ParseIngredient(const nlohmann::json& j) {
            Ingredient ing;
            if (j.is_null()) return ing;
            ing.present = true;
            if (j.is_string()) {
                ing.entries.push_back(j.get<std::string>());
            } else if (j.is_array()) {
                for (const auto& e : j) if (e.is_string()) ing.entries.push_back(e.get<std::string>());
            }
            return ing;
        }

        std::once_flag g_once;
        std::vector<Recipe> g_recipes;

        void Load() {
            std::error_code ec;
            const std::filesystem::path root = DataRoot();
            if (!std::filesystem::is_directory(root, ec)) return;
            std::vector<std::filesystem::path> files;
            for (const auto& ns : std::filesystem::directory_iterator(root, ec)) {
                const std::filesystem::path dir = ns.path() / "recipe";
                if (!std::filesystem::is_directory(dir, ec)) continue;
                for (const auto& f : std::filesystem::recursive_directory_iterator(dir, ec)) {
                    if (f.is_regular_file() && f.path().extension() == ".json") files.push_back(f.path());
                }
            }
            // RecipeManager iterates its map in id order.
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                std::ifstream in(file);
                if (!in) continue;
                nlohmann::json j;
                try { in >> j; } catch (const std::exception&) { continue; }
                if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) continue;
                const std::string type = j["type"].get<std::string>();
                Recipe r;
                if (type == "minecraft:smithing_transform") {
                    r.kind = Recipe::Kind::Transform;
                    if (!j.contains("result") || !j["result"].is_object()) continue;
                    const auto& res = j["result"];
                    std::string id = res.value("id", std::string());
                    if (id.rfind("minecraft:", 0) == 0) id = id.substr(10);
                    r.resultItem = RecipeManager::ItemFromSlug(id);
                    r.resultCount = res.value("count", 1);
                    if (r.resultItem == Items::Air) continue;
                } else if (type == "minecraft:smithing_trim") {
                    r.kind = Recipe::Kind::Trim;
                    r.pattern = j.value("pattern", std::string());
                    if (r.pattern.empty()) continue;
                    if (r.pattern.find(':') == std::string::npos) r.pattern = "minecraft:" + r.pattern;
                } else {
                    continue;
                }
                r.id = file.stem().string();
                r.templ = ParseIngredient(j.contains("template") ? j["template"] : nlohmann::json());
                r.base = ParseIngredient(j.contains("base") ? j["base"] : nlohmann::json());
                r.addition = ParseIngredient(j.contains("addition") ? j["addition"] : nlohmann::json());
                g_recipes.push_back(std::move(r));
            }
            Log::Info("[SmithingRecipes] %zu smithing recipes", g_recipes.size());
        }

        const std::vector<Recipe>& All() {
            std::call_once(g_once, Load);
            return g_recipes;
        }

        // SmithingTrimRecipe.applyTrim.
        ItemStack ApplyTrim(const ItemStack& base, const ItemStack& addition, const std::string& pattern) {
            const auto material = addition.get(DataComponents::PROVIDES_TRIM_MATERIAL);
            if (!material) return ItemStack{};
            ArmorTrim trim{*material, pattern};
            if (const auto existing = base.get(DataComponents::TRIM); existing && *existing == trim) return ItemStack{};
            ItemStack result = base;
            result.count = 1;
            result.components.set(DataComponents::TRIM, std::move(trim));
            return result;
        }

    } // namespace

    ItemStack Assemble(const ItemStack& templateItem, const ItemStack& base, const ItemStack& addition) {
        for (const Recipe& r : All()) {
            if (!r.templ.Test(templateItem) || !r.base.Test(base) || !r.addition.Test(addition)) continue;
            if (r.kind == Recipe::Kind::Trim) return ApplyTrim(base, addition, r.pattern);
            // ItemStackTemplate.apply: base.transmuteCopy(result, count).
            ItemStack result = base;
            result.itemId = r.resultItem;
            result.count = r.resultCount;
            return result;
        }
        return ItemStack{};
    }

    std::string MatchingRecipeId(const ItemStack& templateItem, const ItemStack& base, const ItemStack& addition) {
        for (const Recipe& r : All()) {
            if (!r.templ.Test(templateItem) || !r.base.Test(base) || !r.addition.Test(addition)) continue;
            return "minecraft:" + r.id;
        }
        return {};
    }

    bool IsTemplate(const ItemStack& stack) {
        if (stack.IsEmpty()) return false;
        for (const Recipe& r : All()) if (r.templ.present && r.templ.Test(stack)) return true;
        return false;
    }

    bool IsBase(const ItemStack& stack) {
        if (stack.IsEmpty()) return false;
        for (const Recipe& r : All()) if (r.base.present && r.base.Test(stack)) return true;
        return false;
    }

    bool IsAddition(const ItemStack& stack) {
        if (stack.IsEmpty()) return false;
        for (const Recipe& r : All()) if (r.addition.present && r.addition.Test(stack)) return true;
        return false;
    }

} // namespace Game::SmithingRecipes
