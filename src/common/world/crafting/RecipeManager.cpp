// File: src/common/world/crafting/RecipeManager.cpp
#include "RecipeManager.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <unordered_map>

namespace Game {

    namespace {
        // Resolved ingredient: the sorted ItemIDs that satisfy it. Sorted so
        // membership is a binary search — ingredients like #minecraft:planks
        // carry a dozen items and are tested once per grid cell per recipe.
        using IngredientItems = std::vector<ItemID>;

        std::vector<IngredientItems>            s_ingredients;
        std::vector<CraftingRecipe>             s_recipes;
        std::unordered_map<std::string, ItemID> s_slugToItem;
        bool                                    s_initialized = false;

        // MC Util.isSymmetrical — a pattern equal to its own horizontal mirror
        // needs only one matching pass.
        bool IsSymmetrical(int width, int height, const std::vector<int32_t>& cells) {
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width / 2; ++x) {
                    if (cells[x + y * width] != cells[width - 1 - x + y * width]) {
                        return false;
                    }
                }
            }
            return true;
        }

        // Kuhn's algorithm over the tiny (<=9x9) "which input stack feeds which
        // ingredient" bipartite graph. MC reaches the same answer through
        // StackedContents' flow solver; at this size an augmenting-path search
        // is exact, allocation-light and far less code.
        //
        // `assignment[j]` receives the index into `inputs` that satisfies
        // ingredient j. Returns false unless EVERY ingredient is matched, which
        // — combined with the caller's `inputCount == ingredientCount` check —
        // means every input stack is consumed exactly once.
        bool BipartiteMatch(const std::vector<const ItemStack*>& inputs,
                            const std::vector<int32_t>& cells,
                            const std::vector<std::vector<bool>>& edges,
                            std::vector<int>& assignment) {
            const int n = static_cast<int>(cells.size());
            assignment.assign(n, -1);
            std::vector<int> inputOwner(inputs.size(), -1);

            // Recursive augmenting-path search, written as an explicit lambda so
            // the visited set can be reused across ingredients.
            std::vector<bool> visited(inputs.size(), false);
            struct Augment {
                const std::vector<std::vector<bool>>& edges;
                std::vector<int>& inputOwner;
                std::vector<bool>& visited;
                bool operator()(int ingredient, const Augment& self) const {
                    for (size_t i = 0; i < inputOwner.size(); ++i) {
                        if (!edges[ingredient][i] || visited[i]) continue;
                        visited[i] = true;
                        if (inputOwner[i] == -1 || self(inputOwner[i], self)) {
                            inputOwner[i] = ingredient;
                            return true;
                        }
                    }
                    return false;
                }
            } augment{edges, inputOwner, visited};

            for (int j = 0; j < n; ++j) {
                std::fill(visited.begin(), visited.end(), false);
                if (!augment(j, augment)) return false;
            }
            for (size_t i = 0; i < inputOwner.size(); ++i) {
                if (inputOwner[i] >= 0) assignment[inputOwner[i]] = static_cast<int>(i);
            }
            return true;
        }
    } // namespace

    // ─── CraftingInput ───────────────────────────────────────────
    CraftingInput CraftingInput::OfPositioned(int width, int height,
                                              const std::vector<ItemStack>& items,
                                              int& outLeft, int& outTop) {
        // Verbatim port of CraftingInput.ofPositioned (CraftingInput.java:38-84).
        outLeft = 0;
        outTop  = 0;
        CraftingInput out;
        if (width <= 0 || height <= 0) return out;

        int left = width - 1, right = 0, top = height - 1, bottom = 0;
        for (int y = 0; y < height; ++y) {
            bool rowEmpty = true;
            for (int x = 0; x < width; ++x) {
                if (items[x + y * width].IsEmpty()) continue;
                left  = std::min(left, x);
                right = std::max(right, x);
                rowEmpty = false;
            }
            if (!rowEmpty) {
                top    = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }

        const int newWidth  = right - left + 1;
        const int newHeight = bottom - top + 1;
        if (newWidth <= 0 || newHeight <= 0) return out;   // grid is entirely empty

        outLeft = left;
        outTop  = top;
        out.m_width  = newWidth;
        out.m_height = newHeight;
        out.m_items.reserve(static_cast<size_t>(newWidth) * newHeight);
        for (int y = 0; y < newHeight; ++y) {
            for (int x = 0; x < newWidth; ++x) {
                const ItemStack& stack = items[(x + left) + (y + top) * width];
                if (!stack.IsEmpty()) out.m_ingredientCount++;
                out.m_items.push_back(stack);
            }
        }
        return out;
    }

    const ItemStack& CraftingInput::GetItem(int index) const {
        static const ItemStack kEmpty{};
        if (index < 0 || index >= static_cast<int>(m_items.size())) return kEmpty;
        return m_items[index];
    }

    // ─── Initialization ──────────────────────────────────────────
    void RecipeManager::Initialize() {
        if (s_initialized) return;
        s_initialized = true;

        // Registry slug → ItemID. Block items reuse the BlockID numerically, so
        // the block's model name (which IS its registry slug for every block
        // that came from all_blocks.txt) is the key. State-variant BlockIDs like
        // SnowGrass share a slug with their base block; first wins, which is the
        // default state — and no recipe results in a non-default state anyway.
        s_slugToItem.reserve(static_cast<size_t>(BlockID::Count) + kPureItemTableSize);
        for (int i = 1; i < static_cast<int>(BlockID::Count); ++i) {
            const auto& block = BlockRegistry::Get(static_cast<BlockID>(i));
            if (block.modelName.empty()) continue;
            s_slugToItem.emplace(block.modelName, static_cast<ItemID>(i));
        }
        for (size_t i = 0; i < kPureItemTableSize; ++i) {
            s_slugToItem.emplace(kPureItemTable[i].slug,
                                 PURE_ITEM_BASE + static_cast<ItemID>(i));
        }

        // Resolve the interned ingredient sets. An ingredient whose slugs ALL
        // fail to resolve can never match, which drops every recipe using it.
        s_ingredients.resize(kRecipeIngredientCount);
        for (size_t i = 0; i < kRecipeIngredientCount; ++i) {
            const auto& row = kRecipeIngredients[i];
            auto& out = s_ingredients[i];
            out.reserve(row.count);
            for (uint32_t k = 0; k < row.count; ++k) {
                const ItemID id = ItemFromSlug(kRecipeIngredientSlugs[row.begin + k]);
                if (id != Items::Air) out.push_back(id);
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }

        size_t dropped = 0;
        s_recipes.reserve(kRecipeTableSize);
        for (size_t i = 0; i < kRecipeTableSize; ++i) {
            const RecipeRow& row = kRecipeTable[i];

            const ItemID result = ItemFromSlug(row.resultSlug);
            if (result == Items::Air) { ++dropped; continue; }

            CraftingRecipe recipe;
            recipe.id          = row.id;
            recipe.kind        = static_cast<RecipeKind>(row.kind);
            recipe.width       = row.width;
            recipe.height      = row.height;
            recipe.resultItem  = result;
            recipe.resultCount = row.resultCount;
            recipe.cells.assign(kRecipeCells + row.cellBegin,
                                kRecipeCells + row.cellBegin + row.cellCount);

            bool usable = true;
            for (int32_t cell : recipe.cells) {
                if (cell < 0) continue;             // shaped "must be empty"
                recipe.ingredientCount++;
                if (s_ingredients[cell].empty()) usable = false;
            }
            if (!usable) { ++dropped; continue; }

            if (recipe.kind == RecipeKind::Shaped) {
                recipe.symmetrical = IsSymmetrical(recipe.width, recipe.height, recipe.cells);
            }
            s_recipes.push_back(std::move(recipe));
        }

        Log::Info("[RecipeManager] %zu crafting recipes loaded (%zu dropped — unknown items)",
                  s_recipes.size(), dropped);
    }

    ItemID RecipeManager::ItemFromSlug(const std::string& slug) {
        auto it = s_slugToItem.find(slug);
        return (it != s_slugToItem.end()) ? it->second : Items::Air;
    }

    size_t RecipeManager::RecipeCount() { return s_recipes.size(); }

    // ─── Matching ────────────────────────────────────────────────
    bool RecipeManager::IngredientMatches(int32_t index, const ItemStack& stack) {
        if (index < 0 || index >= static_cast<int32_t>(s_ingredients.size())) return false;
        const auto& items = s_ingredients[index];
        return std::binary_search(items.begin(), items.end(), stack.itemId);
    }

    bool RecipeManager::ShapedMatches(const CraftingRecipe& r, const CraftingInput& in) {
        // MC ShapedRecipePattern.matches: the trimmed input must be exactly the
        // pattern's size, then compare cell by cell — once as authored, and once
        // mirrored unless the pattern is its own mirror.
        if (in.Width() != r.width || in.Height() != r.height) return false;

        auto matchesWithFlip = [&](bool flip) {
            for (int y = 0; y < r.height; ++y) {
                for (int x = 0; x < r.width; ++x) {
                    const int32_t cell = flip ? r.cells[r.width - x - 1 + y * r.width]
                                              : r.cells[x + y * r.width];
                    const ItemStack& actual = in.GetItem(x, y);
                    if (cell < 0) {
                        if (!actual.IsEmpty()) return false;
                    } else if (actual.IsEmpty() || !IngredientMatches(cell, actual)) {
                        return false;
                    }
                }
            }
            return true;
        };

        // MC tries the mirrored orientation first, then the authored one.
        if (!r.symmetrical && matchesWithFlip(true)) return true;
        return matchesWithFlip(false);
    }

    bool RecipeManager::UnorderedMatches(const CraftingRecipe& r, const CraftingInput& in) {
        // MC ShapelessRecipe.matches. The one-ingredient case is a direct test;
        // beyond that every input stack must pair with a distinct ingredient,
        // which is a perfect bipartite matching.
        std::vector<const ItemStack*> inputs;
        inputs.reserve(in.IngredientCount());
        for (const auto& stack : in.Items()) {
            if (!stack.IsEmpty()) inputs.push_back(&stack);
        }
        if (inputs.size() != r.cells.size()) return false;
        if (inputs.size() == 1) return IngredientMatches(r.cells[0], *inputs[0]);

        std::vector<std::vector<bool>> edges(r.cells.size(),
                                             std::vector<bool>(inputs.size(), false));
        for (size_t j = 0; j < r.cells.size(); ++j) {
            for (size_t i = 0; i < inputs.size(); ++i) {
                edges[j][i] = IngredientMatches(r.cells[j], *inputs[i]);
            }
        }
        std::vector<int> assignment;
        return BipartiteMatch(inputs, r.cells, edges, assignment);
    }

    const CraftingRecipe* RecipeManager::Find(const CraftingInput& input) {
        if (input.IsEmpty()) return nullptr;

        for (const auto& recipe : s_recipes) {
            // MC's first and cheapest rejection, shared by both pattern kinds.
            if (recipe.ingredientCount != input.IngredientCount()) continue;
            const bool matched = (recipe.kind == RecipeKind::Shaped)
                               ? ShapedMatches(recipe, input)
                               : UnorderedMatches(recipe, input);
            if (matched) return &recipe;
        }
        return nullptr;
    }

    ItemStack RecipeManager::Assemble(const CraftingRecipe& recipe, const CraftingInput& input) {
        ItemStack out{recipe.resultItem, recipe.resultCount};

        if (recipe.kind != RecipeKind::Transmute) return out;

        // MC TransmuteRecipe.assemble: the result carries the INPUT stack's
        // components (a dyed bundle keeps whatever it was holding). Ingredient
        // 0 is the donor; re-run the pairing to find which stack filled it,
        // because the two ingredients can in principle accept the same item.
        std::vector<const ItemStack*> inputs;
        for (const auto& stack : input.Items()) {
            if (!stack.IsEmpty()) inputs.push_back(&stack);
        }
        if (inputs.size() != recipe.cells.size()) return out;

        std::vector<std::vector<bool>> edges(recipe.cells.size(),
                                             std::vector<bool>(inputs.size(), false));
        for (size_t j = 0; j < recipe.cells.size(); ++j) {
            for (size_t i = 0; i < inputs.size(); ++i) {
                edges[j][i] = IngredientMatches(recipe.cells[j], *inputs[i]);
            }
        }
        std::vector<int> assignment;
        if (BipartiteMatch(inputs, recipe.cells, edges, assignment)
            && assignment[0] >= 0) {
            ItemStack donor = *inputs[assignment[0]];
            donor.itemId = recipe.resultItem;
            donor.count  = recipe.resultCount;
            return donor;                 // components ride along
        }
        return out;
    }

    std::vector<ItemStack> RecipeManager::GetRemainingItems(const CraftingInput& input) {
        // MC CraftingRecipe.defaultCraftingReminder — no crafting recipe kind we
        // support overrides getRemainingItems, so this IS the answer for all of
        // them: each cell leaves behind its item's crafting remainder.
        std::vector<ItemStack> out;
        out.reserve(input.Items().size());
        for (const auto& stack : input.Items()) {
            const ItemID remainder = stack.IsEmpty()
                ? Items::Air
                : ItemRegistry::Get(stack.itemId).craftingRemainder;
            out.push_back(remainder == Items::Air ? ItemStack{} : ItemStack{remainder, 1});
        }
        return out;
    }

} // namespace Game
