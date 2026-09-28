// File: src/common/world/crafting/RecipeManager.cpp
#include "RecipeManager.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Features.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/world/map/MapItem.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/entity/DyeColorUtil.hpp"
#include "common/entity/FireworkItems.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace Game {

    namespace {
        constexpr const char* kTippedArrowRecipeId = "minecraft:tipped_arrow";

        // MC 26.3 BookCloningRecipe "minecraft:book_cloning"
        // (VanillaRecipeProvider: source WRITTEN_BOOK, material WRITABLE_BOOK,
        // allowed generations 0..1, result WRITTEN_BOOK). A custom recipe:
        // one written book plus any number of books and quills, anywhere in
        // the grid. The data pack's crafting_special_bookcloning JSON is not
        // a type the generator bakes, so it is matched here.
        constexpr const char* kBookCloningRecipeId = "minecraft:book_cloning";
        CraftingRecipe s_bookCloning = [] {
            CraftingRecipe r;
            r.id = kBookCloningRecipeId;
            r.kind = RecipeKind::Shapeless;
            r.resultItem = Items::WrittenBook;
            r.resultCount = 1;
            return r;
        }();

        // BookCloningRecipe.matches: at least two items; exactly one source
        // (a written book whose generation is in 0..1); every other item a
        // material (a book and quill).
        bool BookCloningMatches(const CraftingInput& input) {
            if (input.IngredientCount() < 2) return false;
            bool hasSource = false, hasMaterial = false;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (stack.itemId == Items::WrittenBook) {
                    const auto content = stack.get(DataComponents::WRITTEN_BOOK_CONTENT);
                    if (!content || content->generation < 0 || content->generation > 1) return false;
                    if (hasSource) return false;
                    hasSource = true;
                } else if (stack.itemId == Items::WritableBook) {
                    hasMaterial = true;
                } else {
                    return false;
                }
            }
            return hasSource && hasMaterial;
        }

        // MC 26.3 "minecraft:map_cloning" — a TransmuteRecipe (input
        // #minecraft:clonable_maps, material minecraft:map, material_count
        // 1..8, add_material_count_to_result, result: the input item): one
        // map and 1–8 blank maps give 1 + n copies carrying the map's
        // components. Hand-coded like book cloning; the data pack's
        // map_cloning.json keeps the crafting_special type the recipe
        // generator skips, so the two can never both match.
        constexpr const char* kMapCloningRecipeId = "minecraft:map_cloning";
        CraftingRecipe s_mapCloning = [] {
            CraftingRecipe r;
            r.id = kMapCloningRecipeId;
            r.kind = RecipeKind::Shapeless;
            r.resultItem = Items::FilledMap;
            r.resultCount = 1;
            return r;
        }();

        // TransmuteRecipe.matches for map_cloning.
        bool MapCloningMatches(const CraftingInput& input, int* outMaterials = nullptr,
                               const ItemStack** outMap = nullptr) {
            constexpr int kMinMaterials = 1, kMaxMaterials = 8;
            if (input.IngredientCount() < kMinMaterials + 1 || input.IngredientCount() > kMaxMaterials + 1) return false;
            const ItemStack* found = nullptr;
            int materials = 0;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (MapItem::IsMapItem(stack.itemId)) {        // #minecraft:clonable_maps
                    if (found) return false;
                    found = &stack;
                } else if (stack.itemId == Items::Map) {
                    if (++materials > kMaxMaterials) return false;
                } else {
                    return false;
                }
            }
            if (!found || materials < kMinMaterials) return false;
            if (outMaterials) *outMaterials = materials;
            if (outMap) *outMap = found;
            return true;   // result size 1 + n is never 1: no "unchanged" check needed
        }

        // MC 26.3 MapExtendingRecipe "minecraft:map_extending": paper ring
        // around a #minecraft:extendable_maps map (filled_map) whose data is
        // below the maximum scale. The result is the map with
        // MAP_POST_PROCESSING SCALE; taking it gives it its new, zoomed-out
        // map (MapItem.onCraftedPostProcess).
        constexpr const char* kMapExtendingRecipeId = "minecraft:map_extending";
        CraftingRecipe s_mapExtending = [] {
            CraftingRecipe r;
            r.id = kMapExtendingRecipeId;
            r.kind = RecipeKind::Shaped;
            r.resultItem = Items::FilledMap;
            r.resultCount = 1;
            return r;
        }();

        bool MapExtendingMatches(const CraftingInput& input) {
            // ShapedRecipePattern "###", "#x#", "###".
            if (input.Width() != 3 || input.Height() != 3) return false;
            for (int y = 0; y < 3; ++y) {
                for (int x = 0; x < 3; ++x) {
                    const ItemStack& stack = input.GetItem(x, y);
                    if (x == 1 && y == 1) {
                        if (stack.IsEmpty() || stack.itemId != Items::FilledMap) return false;
                    } else if (stack.IsEmpty() || stack.itemId != Items::Paper) {
                        return false;
                    }
                }
            }
            // findFilledMap: the stack with a MAP_ID; its data must exist and
            // be below scale 4.
            const ItemStack& map = input.GetItem(1, 1);
            if (!map.get(DataComponents::MAP_ID)) return false;
            const std::optional<int> scale = MapItemBridge::MapScale(map);
            return scale && *scale < 4;
        }

        // ── The fireworks' custom recipes (MC 26.3 FireworkRocketRecipe,
        // FireworkStarRecipe, FireworkStarFadeRecipe — VanillaRecipeProvider
        // gives them their ingredients). Their data-pack JSONs keep the
        // crafting_special types the recipe generator skips, so they are
        // matched here; they are tried AHEAD of the generated table, where
        // "firework_rocket_simple" (paper + gunpowder, no components) would
        // otherwise answer the one-gunpowder rocket — vanilla's special
        // recipe sorts first and gives the flight-1 rocket.
        constexpr const char* kFireworkRocketRecipeId   = "minecraft:firework_rocket";
        constexpr const char* kFireworkStarRecipeId     = "minecraft:firework_star";
        constexpr const char* kFireworkStarFadeRecipeId = "minecraft:firework_star_fade";
        CraftingRecipe s_fireworkRocket = [] {
            CraftingRecipe r;
            r.id = kFireworkRocketRecipeId;
            r.kind = RecipeKind::Shapeless;
            r.resultItem = Items::FireworkRocket;
            r.resultCount = 3;
            return r;
        }();
        CraftingRecipe s_fireworkStar = [] {
            CraftingRecipe r;
            r.id = kFireworkStarRecipeId;
            r.kind = RecipeKind::Shapeless;
            r.resultItem = Items::FireworkStar;
            r.resultCount = 1;
            return r;
        }();
        CraftingRecipe s_fireworkStarFade = [] {
            CraftingRecipe r;
            r.id = kFireworkStarFadeRecipeId;
            r.kind = RecipeKind::Shapeless;
            r.resultItem = Items::FireworkStar;
            r.resultCount = 1;
            return r;
        }();

        // DecoratedPotRecipe (crafting_decorated_pot, VanillaRecipeProvider:
        // every side #decorated_pot_ingredients — bricks and sherds). Its
        // JSON is a special type the recipe generator skips.
        CraftingRecipe s_decoratedPot = [] {
            CraftingRecipe r;
            r.id = "minecraft:decorated_pot";
            r.kind = RecipeKind::Shaped;
            r.width = 3;
            r.height = 3;
            r.resultItem = ItemRegistry::FromBlock(BlockID::DecoratedPot);
            r.resultCount = 1;
            return r;
        }();

        bool IsPotIngredient(const ItemStack& stack) {
            return !stack.IsEmpty() &&
                   DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId),
                                    "minecraft:decorated_pot_ingredients");
        }

        // DecoratedPotRecipe.matches: a 3x3 grid with exactly the four edge
        // centres filled — back (1,0), left (0,1), right (2,1), front (1,2).
        bool DecoratedPotMatches(const CraftingInput& input) {
            if (input.Width() != 3 || input.Height() != 3 || input.IngredientCount() != 4) return false;
            return IsPotIngredient(input.GetItem(1, 0)) && IsPotIngredient(input.GetItem(0, 1)) &&
                   IsPotIngredient(input.GetItem(2, 1)) && IsPotIngredient(input.GetItem(1, 2));
        }

        // DataComponents.DYE — the sixteen dyes (#minecraft:dyes).
        bool IsDye(const ItemStack& stack) { return DyeColorOfItem(stack.itemId) >= 0; }

        // FireworkStarRecipe.findShape: fire charge → LARGE_BALL, feather →
        // BURST, gold nugget → STAR, #minecraft:skulls → CREEPER.
        std::optional<FireworkExplosion::Shape> FireworkShapeOf(const ItemStack& stack) {
            if (stack.itemId == Items::FireCharge) return FireworkExplosion::Shape::LargeBall;
            if (stack.itemId == Items::Feather)    return FireworkExplosion::Shape::Burst;
            if (stack.itemId == Items::GoldNugget) return FireworkExplosion::Shape::Star;
            if (DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId), "minecraft:skulls")) {
                return FireworkExplosion::Shape::Creeper;
            }
            return std::nullopt;
        }

        // FireworkRocketRecipe.matches: one paper, one to three gunpowder,
        // and nothing else but firework stars.
        bool FireworkRocketMatches(const CraftingInput& input) {
            if (input.IngredientCount() < 2) return false;
            bool hasShell = false;
            int fuelCount = 0;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (stack.itemId == Items::Paper) {
                    if (hasShell) return false;
                    hasShell = true;
                } else if (stack.itemId == Items::Gunpowder) {
                    if (++fuelCount > 3) return false;
                } else if (stack.itemId != Items::FireworkStar) {
                    return false;
                }
            }
            return hasShell && fuelCount >= 1;
        }

        // FireworkStarRecipe.matches: exactly one gunpowder, at least one dye,
        // at most one each of glowstone dust (twinkle), diamond (trail) and
        // shape ingredient, nothing else.
        bool FireworkStarMatches(const CraftingInput& input) {
            if (input.IngredientCount() < 2) return false;
            bool hasFuel = false, hasDye = false, hasShape = false, hasTrail = false, hasTwinkle = false;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (stack.itemId == Items::GlowstoneDust) {
                    if (hasTwinkle) return false;
                    hasTwinkle = true;
                } else if (stack.itemId == Items::Diamond) {
                    if (hasTrail) return false;
                    hasTrail = true;
                } else if (stack.itemId == Items::Gunpowder) {
                    if (hasFuel) return false;
                    hasFuel = true;
                } else if (IsDye(stack)) {
                    hasDye = true;
                } else {
                    if (!FireworkShapeOf(stack) || hasShape) return false;
                    hasShape = true;
                }
            }
            return hasFuel && hasDye;
        }

        // FireworkStarFadeRecipe.matches: one firework star and any dyes.
        bool FireworkStarFadeMatches(const CraftingInput& input) {
            if (input.IngredientCount() < 2) return false;
            bool hasDye = false, hasTarget = false;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (IsDye(stack)) {
                    hasDye = true;
                } else {
                    if (stack.itemId != Items::FireworkStar || hasTarget) return false;
                    hasTarget = true;
                }
            }
            return hasTarget && hasDye;
        }

        // Resolved ingredient: the sorted ItemIDs that satisfy it. Sorted so
        // membership is a binary search — ingredients like #minecraft:planks
        // carry a dozen items and are tested once per grid cell per recipe.
        using IngredientItems = std::vector<ItemID>;

        std::vector<IngredientItems>            s_ingredients;
        std::vector<CraftingRecipe>             s_recipes;
        std::vector<CookingRecipe>              s_cookingRecipes;
        std::vector<StonecuttingRecipe>         s_stonecuttingRecipes;
        std::unordered_map<ItemID, int>         s_fuelBurnTimes;
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

        // Registry slug → ItemID. Block items reuse the BlockID numerically,
        // so the block's registry slug is the key. State-variant BlockIDs like
        // SnowGrass share a slug with their base block; first wins, which is
        // the default state — and no recipe results in a non-default state
        // anyway.
        //
        // Keyed on registrySlug, NOT modelName: modelName is a rendering
        // detail that several blocks share, because Init() re-registers some
        // blocks with a borrowed model. That made this map resolve "stone" to
        // InfestedStone, "stone_bricks" to InfestedStoneBricks and
        // "mossy_stone_bricks" to InfestedMossyStoneBricks — all three
        // Infested* rows sort earlier in BlockDefs.inc, and emplace keeps the
        // first — so every recipe naming them produced the silverfish block.
        s_slugToItem.reserve(static_cast<size_t>(BlockID::Count) + kPureItemTableSize);
        for (int i = 1; i < static_cast<int>(BlockID::Count); ++i) {
            const auto& block = BlockRegistry::Get(static_cast<BlockID>(i));
            if (block.registrySlug.empty()) continue;
            s_slugToItem.emplace(block.registrySlug, static_cast<ItemID>(i));
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

        // ── The engine's own items ──────────────────────────────────────────
        //
        // The portal gun and the portal wand have no data-pack row (they
        // are not Minecraft items), so their recipes are written here in the
        // shape MC's ShapedRecipe JSON takes: a pattern and a key. Registered
        // AFTER the generated table so a pack can never shadow them, and
        // only when the item exists in this build (its id is assigned at
        // ItemRegistry::Initialize; 0 = the feature is off).
        const auto shaped = [&](const char* id, ItemID result,
                                const std::vector<const char*>& pattern,
                                const std::vector<std::pair<char, ItemID>>& key) {
            if (result == Items::Air) return;
            CraftingRecipe recipe;
            recipe.id          = id;
            recipe.kind        = RecipeKind::Shaped;
            recipe.height      = static_cast<int>(pattern.size());
            recipe.width       = static_cast<int>(std::strlen(pattern.front()));
            recipe.resultItem  = result;
            recipe.resultCount = 1;
            std::unordered_map<char, int32_t> slots;
            for (const auto& [symbol, item] : key) {
                if (item == Items::Air) return;   // an ingredient this build lacks
                slots[symbol] = static_cast<int32_t>(s_ingredients.size());
                s_ingredients.push_back(IngredientItems{ item });
            }
            for (const char* row : pattern) {
                for (int x = 0; x < recipe.width; ++x) {
                    const char c = row[x];
                    if (c == ' ') { recipe.cells.push_back(-1); continue; }
                    const auto it = slots.find(c);
                    if (it == slots.end()) return;   // a pattern symbol with no key
                    recipe.cells.push_back(it->second);
                    recipe.ingredientCount++;
                }
            }
            recipe.symmetrical = IsSymmetrical(recipe.width, recipe.height, recipe.cells);
            s_recipes.push_back(std::move(recipe));
        };
        const ItemID obsidian = ItemRegistry::FromBlock(BlockID::Obsidian);
#if ENABLE_PORTAL_GUN
        // Obsidian body, iron fittings, a diamond at the heart, an iron grip.
        shaped("obeycraft:portal_gun", Items::PortalGun,
               { "OOO",
                 "IDI",
                 " I " },
               { { 'O', obsidian }, { 'I', Items::IronIngot }, { 'D', Items::Diamond } });
#endif
#if ENABLE_IMMERSIVE_PORTALS
        // A blaze rod with an ender pearl at its tip.
        shaped("obeycraft:portal_wand", Items::PortalWand,
               { " E",
                 "B " },
               { { 'E', Items::EnderPearl }, { 'B', Items::BlazeRod } });
#endif
        // MC 26.3 ImbueRecipe "minecraft:tipped_arrow" (VanillaRecipeProvider:
        // source LINGERING_POTION, material ARROW, result TIPPED_ARROW x8).
        // Its matches() is exactly a full 3x3 shaped test — the source in the
        // centre, the material in the other eight cells — so it rides the
        // shaped matcher; Assemble copies the centre's POTION_CONTENTS. (The
        // data pack's old crafting_special_tippedarrow JSON is not a type the
        // generator bakes, which is why it is registered here.)
        shaped(kTippedArrowRecipeId, Items::TippedArrow,
               { "AAA",
                 "ALA",
                 "AAA" },
               { { 'A', Items::Arrow }, { 'L', Items::LingeringPotion } });
        if (!s_recipes.empty() && std::strcmp(s_recipes.back().id, kTippedArrowRecipeId) == 0) {
            s_recipes.back().resultCount = 8;
        }

        Log::Info("[RecipeManager] %zu crafting recipes loaded (%zu dropped — unknown items)",
                  s_recipes.size(), dropped);

        // ── Furnace family ────────────────────────────────────────────────
        size_t cookingDropped = 0;
        s_cookingRecipes.reserve(kCookingRecipeTableSize);
        for (size_t i = 0; i < kCookingRecipeTableSize; ++i) {
            const auto& row = kCookingRecipeTable[i];
            CookingRecipe recipe;
            recipe.id          = row.id;
            recipe.kind        = static_cast<CookingKind>(row.kind);
            recipe.ingredient  = static_cast<int32_t>(row.ingredient);
            recipe.resultItem  = ItemFromSlug(row.resultSlug);
            recipe.resultCount = row.resultCount;
            recipe.cookingTime = row.cookingTime;
            recipe.experience  = row.experience;
            // Same drop rules as a crafting recipe: an unknown result, or an
            // ingredient set that resolved to nothing, can never fire.
            if (recipe.resultItem == Items::Air ||
                recipe.ingredient < 0 ||
                recipe.ingredient >= static_cast<int32_t>(s_ingredients.size()) ||
                s_ingredients[recipe.ingredient].empty()) {
                ++cookingDropped;
                continue;
            }
            s_cookingRecipes.push_back(recipe);
        }

        // ── Stonecutter ───────────────────────────────────────────────────
        size_t cutDropped = 0;
        s_stonecuttingRecipes.reserve(kStonecuttingTableSize);
        for (size_t i = 0; i < kStonecuttingTableSize; ++i) {
            const auto& row = kStonecuttingTable[i];
            StonecuttingRecipe recipe;
            recipe.ingredient  = static_cast<int32_t>(row.ingredient);
            recipe.resultItem  = ItemFromSlug(row.resultSlug);
            recipe.resultCount = row.resultCount;
            if (recipe.resultItem == Items::Air ||
                recipe.ingredient < 0 ||
                recipe.ingredient >= static_cast<int32_t>(s_ingredients.size()) ||
                s_ingredients[recipe.ingredient].empty()) {
                ++cutDropped;
                continue;
            }
            s_stonecuttingRecipes.push_back(recipe);
        }
        Log::Info("[RecipeManager] %zu stonecutting recipes (%zu dropped)",
                  s_stonecuttingRecipes.size(), cutDropped);

        // ── Fuel ──────────────────────────────────────────────────────────
        size_t fuelDropped = 0;
        s_fuelBurnTimes.reserve(kFuelTableSize);
        for (size_t i = 0; i < kFuelTableSize; ++i) {
            const ItemID id = ItemFromSlug(kFuelTable[i].slug);
            if (id == Items::Air) { ++fuelDropped; continue; }
            s_fuelBurnTimes.emplace(id, kFuelTable[i].burnTicks);
        }

        Log::Info("[RecipeManager] %zu cooking recipes, %zu fuels "
                  "(%zu / %zu dropped — unknown items)",
                  s_cookingRecipes.size(), s_fuelBurnTimes.size(),
                  cookingDropped, fuelDropped);
    }

    const CookingRecipe* RecipeManager::FindCooking(CookingKind kind, const ItemStack& input) {
        if (input.IsEmpty()) return nullptr;
        // Linear over ~116 rows, filtered by kind first. MC keeps a per-type
        // list and scans it the same way; a hash index would save nothing at
        // this size and only fires once per furnace state change, not per tick.
        for (const CookingRecipe& recipe : s_cookingRecipes) {
            if (recipe.kind != kind) continue;
            if (IngredientMatches(recipe.ingredient, input)) return &recipe;
        }
        return nullptr;
    }

    int RecipeManager::GetFuelBurnTime(const ItemStack& stack) {
        if (stack.IsEmpty()) return 0;
        auto it = s_fuelBurnTimes.find(stack.itemId);
        return (it != s_fuelBurnTimes.end()) ? it->second : 0;
    }

    size_t RecipeManager::CookingRecipeCount() { return s_cookingRecipes.size(); }

    std::vector<const StonecuttingRecipe*> RecipeManager::FindStonecutting(const ItemStack& input) {
        std::vector<const StonecuttingRecipe*> out;
        if (input.IsEmpty()) return out;
        for (const StonecuttingRecipe& recipe : s_stonecuttingRecipes) {
            if (IngredientMatches(recipe.ingredient, input)) out.push_back(&recipe);
        }
        return out;
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

        // The fireworks first (see s_fireworkRocket): none of the three can
        // match what any table recipe but firework_rocket_simple does.
        if (FireworkRocketMatches(input))   return &s_fireworkRocket;
        if (FireworkStarMatches(input))     return &s_fireworkStar;
        if (FireworkStarFadeMatches(input)) return &s_fireworkStarFade;

        for (const auto& recipe : s_recipes) {
            // MC's first and cheapest rejection, shared by both pattern kinds.
            if (recipe.ingredientCount != input.IngredientCount()) continue;
            const bool matched = (recipe.kind == RecipeKind::Shaped)
                               ? ShapedMatches(recipe, input)
                               : UnorderedMatches(recipe, input);
            if (matched) return &recipe;
        }
        if (DecoratedPotMatches(input)) return &s_decoratedPot;
        if (BookCloningMatches(input)) return &s_bookCloning;
        if (MapCloningMatches(input)) return &s_mapCloning;
        if (MapExtendingMatches(input)) return &s_mapExtending;
        return nullptr;
    }

    ItemStack RecipeManager::Assemble(const CraftingRecipe& recipe, const CraftingInput& input) {
        ItemStack out{recipe.resultItem, recipe.resultCount};

        // BookCloningRecipe.assemble: one copy per book and quill, carrying
        // the source's components (TransmuteRecipe.createWithOriginalComponents)
        // and its content one generation on (craftCopy).
        if (&recipe == &s_bookCloning) {
            const ItemStack* source = nullptr;
            int count = 0;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (stack.itemId == Items::WrittenBook && stack.get(DataComponents::WRITTEN_BOOK_CONTENT)) {
                    if (source) return ItemStack{};
                    source = &stack;
                } else if (stack.itemId == Items::WritableBook) {
                    ++count;
                } else {
                    return ItemStack{};
                }
            }
            if (!source) return ItemStack{};
            const auto content = source->get(DataComponents::WRITTEN_BOOK_CONTENT);
            ItemStack copy = *source;
            copy.itemId = Items::WrittenBook;
            copy.count  = recipe.resultCount + (count - 1);
            copy.components.set(DataComponents::WRITTEN_BOOK_CONTENT, content->CraftCopy());
            return copy;
        }

        // TransmuteRecipe.assemble (map_cloning): the map's item and
        // components, 1 + one per blank map.
        if (&recipe == &s_mapCloning) {
            int materials = 0;
            const ItemStack* map = nullptr;
            if (!MapCloningMatches(input, &materials, &map)) return ItemStack{};
            ItemStack copy = *map;
            copy.count = 1 + materials;
            return copy;
        }

        // MapExtendingRecipe.assemble: the map (createWithOriginalComponents)
        // flagged for the SCALE post-process.
        if (&recipe == &s_mapExtending) {
            ItemStack copy = input.GetItem(1, 1);
            copy.count = 1;
            copy.components.set(DataComponents::MAP_POST_PROCESSING, Maps::MapPostProcessing::Scale);
            return copy;
        }

        // DecoratedPotRecipe.assemble: the pot with POT_DECORATIONS from the
        // four edges (back, left, right, front).
        if (&recipe == &s_decoratedPot) {
            PotDecorations decorations;
            decorations.sides = { input.GetItem(1, 0).itemId, input.GetItem(0, 1).itemId,
                                  input.GetItem(2, 1).itemId, input.GetItem(1, 2).itemId };
            out.components.set(DataComponents::POT_DECORATIONS, decorations);
            return out;
        }

        // FireworkRocketRecipe.assemble: three rockets whose FIREWORKS is
        // the gunpowder count and every star's explosion, in grid order.
        if (&recipe == &s_fireworkRocket) {
            Fireworks fireworks;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (stack.itemId == Items::Gunpowder) {
                    ++fireworks.flightDuration;
                } else if (stack.itemId == Items::FireworkStar) {
                    if (auto explosion = stack.get(DataComponents::FIREWORK_EXPLOSION)) {
                        fireworks.explosions.push_back(*explosion);
                    }
                }
            }
            FireworkItems::SetFireworks(out, fireworks);
            return out;
        }
        // FireworkStarRecipe.assemble: the shape (SMALL_BALL without one),
        // every dye's firework colour in grid order, trail and twinkle.
        if (&recipe == &s_fireworkStar) {
            FireworkExplosion explosion;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (auto shape = FireworkShapeOf(stack)) {
                    explosion.shape = *shape;
                } else if (stack.itemId == Items::GlowstoneDust) {
                    explosion.hasTwinkle = true;
                } else if (stack.itemId == Items::Diamond) {
                    explosion.hasTrail = true;
                } else if (IsDye(stack)) {
                    explosion.colors.push_back(FireworkItems::DyeFireworkColor(
                        static_cast<uint8_t>(DyeColorOfItem(stack.itemId))));
                }
            }
            out.components.set(DataComponents::FIREWORK_EXPLOSION, std::move(explosion));
            return out;
        }
        // FireworkStarFadeRecipe.assemble: the star (its components kept,
        // TransmuteRecipe.createWithOriginalComponents) with the dyes'
        // colours as its fade colours.
        if (&recipe == &s_fireworkStarFade) {
            std::vector<int32_t> colors;
            const ItemStack* target = nullptr;
            for (const ItemStack& stack : input.Items()) {
                if (stack.IsEmpty()) continue;
                if (IsDye(stack)) {
                    colors.push_back(FireworkItems::DyeFireworkColor(
                        static_cast<uint8_t>(DyeColorOfItem(stack.itemId))));
                } else if (stack.itemId == Items::FireworkStar) {
                    target = &stack;
                }
            }
            if (!target || colors.empty()) return ItemStack{};
            ItemStack star = *target;
            star.itemId = recipe.resultItem;
            star.count = recipe.resultCount;
            FireworkExplosion explosion = star.get(DataComponents::FIREWORK_EXPLOSION).value_or(FireworkExplosion{});
            explosion.fadeColors = std::move(colors);
            star.components.set(DataComponents::FIREWORK_EXPLOSION, std::move(explosion));
            return star;
        }

        // ImbueRecipe.assemble: result.set(POTION_CONTENTS, source's).
        if (std::strcmp(recipe.id, kTippedArrowRecipeId) == 0) {
            if (input.Width() == 3 && input.Height() == 3) {
                if (auto contents = input.GetItem(1, 1).get(DataComponents::POTION_CONTENTS)) {
                    out.components.set(DataComponents::POTION_CONTENTS, *contents);
                }
            }
            return out;
        }

        // The suspicious stews are shapeless recipes whose RESULT carries
        // components (data/minecraft/recipe/suspicious_stew_from_<flower>
        // .json: result.components."minecraft:suspicious_stew_effects").
        // The generated table keeps no result components, and each of those
        // lists is exactly the flower's FlowerBlock.getSuspiciousEffects, so
        // the flower named by the recipe id supplies it.
        constexpr std::string_view kStewPrefix = "minecraft:suspicious_stew_from_";
        if (std::string_view(recipe.id).rfind(kStewPrefix, 0) == 0) {
            const std::string flowerSlug(std::string_view(recipe.id).substr(kStewPrefix.size()));
            const ItemID flower = ItemFromSlug(flowerSlug);
            if (ItemRegistry::IsBlockItem(flower)) {
                if (const SuspiciousStewEffects* effects =
                        GetFlowerSuspiciousEffects(ItemRegistry::ToBlock(flower))) {
                    out.components.set(DataComponents::SUSPICIOUS_STEW_EFFECTS, *effects);
                }
            }
            return out;
        }

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
        // BookCloningRecipe.getRemainingItems: the source book stays in the
        // grid (one of it), the rest leave their crafting remainders.
        if (Find(input) == &s_bookCloning) {
            std::vector<ItemStack> out(input.Items().size());
            for (size_t i = 0; i < input.Items().size(); ++i) {
                const ItemStack& stack = input.Items()[i];
                if (stack.IsEmpty()) continue;
                const ItemID remainder = ItemRegistry::Get(stack.itemId).craftingRemainder;
                if (remainder != Items::Air) {
                    out[i] = ItemStack{remainder, 1};
                } else if (stack.get(DataComponents::WRITTEN_BOOK_CONTENT)) {
                    out[i] = stack;
                    out[i].count = 1;
                    break;
                }
            }
            return out;
        }

        // MC CraftingRecipe.defaultCraftingReminder — every other recipe kind
        // we support leaves each cell's crafting remainder behind.
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
