// File: src/client/renderer/gui/CreativeModeTabs.cpp
//
// MC CreativeModeTabs (26.3): the tab registry, each tab's displayItems
// generator run against the engine's item registry, and the search tab built
// tab by tab. See CreativeModeTabs.hpp.
#include "CreativeModeTabs.hpp"

#include "common/entity/FireworkItems.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Instruments.hpp"
#include "common/core/Features.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/entity/raid/OminousBanner.hpp"
#include "common/entity/decoration/PaintingVariants.hpp"
#include "common/text/Language.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"
#include "common/world/block/RedstonePlus.hpp"
#include "common/world/portal/PortalState.hpp"
#include "platform/GameDirectory.hpp"
#include "server/IntegratedServer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Render {

    namespace CreativeTabData {

        enum class EntryKind : uint8_t {
            Item,               // one plain stack of the id
            EnchantedItem,      // the id with ENCHANTMENTS "slug:level,..." (TF gear)
            PresetPaintings,    // generatePresetPaintings(#placeable)
            OpPaintings,        // generatePresetPaintings(!#placeable)
            Fireworks,          // generateFireworksAllDurations
            Instruments,        // generateInstrumentTypes (extra = instrument tag)
            SuspiciousStews,    // generateSuspiciousStews
            OminousBottles,     // generateOminousBottles
            Potions,            // generatePotionEffectTypes(item)
            EnchantedBooksMax,  // generateEnchantmentBookTypesOnlyMaxLevel
            EnchantedBooksAll,  // generateEnchantmentBookTypesAllLevels
            OminousBanner,      // Raid.getOminousBannerInstance
            SwetBanner,         // AetherItems.createSwetBannerItemStack
            TestBlockModes,     // TestBlock.setModeOnStack per TestBlockMode
            LightLevels,        // LightBlock.setLightOnStack 15..0
        };

        // MC CreativeModeTab.TabVisibility.
        enum class Visibility : uint8_t { ParentAndSearch, ParentOnly, SearchOnly };

        using Row     = CreativeModeTab::Row;
        using TabType = CreativeModeTab::Type;

        struct CreativeEntry {
            EntryKind   kind;
            const char* id;       // registry path; nullptr only in an empty table's filler row
            Visibility  visibility;
            const char* extra;    // kind-specific argument, or nullptr
        };

        struct VanillaTabDef {
            const char*          key;
            Row                  row;
            int                  column;
            const char*          titleKey;
            const char*          icon;
            TabType              type;
            bool                 alignedRight;
            bool                 showTitle;
            bool                 canScroll;
            const char*          background;   // tab_<background>.png
            bool                 requiresPermissions;
            const CreativeEntry* entries;
            size_t               entryCount;
        };

        struct ModInsert {
            const char* tab;      // vanilla tab key
            const char* after;    // registry path of the anchor
            const char* item;     // registry path of the inserted item
        };

#include "GeneratedCreativeModeTabs.inc"

    } // namespace CreativeTabData

    namespace {

        using namespace CreativeTabData;

        constexpr const char* kBackgroundDir = "assets/textures/gui/container/creative_inventory/tab_";

        // ── Engine content in the vanilla tabs ──────────────────────────────
        // Items that are neither vanilla nor the Aether / Twilight Forest,
        // each inserted right after `after` in `tab` — where Mojang files the
        // nearest vanilla kin. Rows sharing an anchor keep this table's order.
        struct Placement {
            const char* tab;
            const char* after;
            const char* item;
        };

        const Placement kEnginePlacements[] = {
            // Building blocks: the Hush's whisperwood set follows the last
            // wood set (the nether stems) as a dimension's wood; its stone
            // sets (hushstone, Aurelith's choirstone and stave stone) follow
            // the End's purpur the way the End follows the Nether; resonite
            // joins the metal blocks after netherite.
            {"building_blocks", "warped_button", "whisperwood_log"},
            {"building_blocks", "warped_button", "stripped_whisperwood_log"},
            {"building_blocks", "warped_button", "whisperwood_planks"},
            {"building_blocks", "warped_button", "whisperwood_stairs"},
            {"building_blocks", "warped_button", "whisperwood_slab"},
            {"building_blocks", "warped_button", "whisperwood_fence"},
            {"building_blocks", "warped_button", "whisperwood_fence_gate"},
            {"building_blocks", "warped_button", "whisperwood_door"},
            {"building_blocks", "warped_button", "whisperwood_trapdoor"},
            {"building_blocks", "purpur_slab", "hushstone"},
            {"building_blocks", "purpur_slab", "hushstone_stairs"},
            {"building_blocks", "purpur_slab", "hushstone_slab"},
            {"building_blocks", "purpur_slab", "polished_hushstone"},
            {"building_blocks", "purpur_slab", "polished_hushstone_stairs"},
            {"building_blocks", "purpur_slab", "polished_hushstone_slab"},
            {"building_blocks", "purpur_slab", "hushstone_bricks"},
            {"building_blocks", "purpur_slab", "cracked_hushstone_bricks"},
            {"building_blocks", "purpur_slab", "hushstone_brick_stairs"},
            {"building_blocks", "purpur_slab", "hushstone_brick_slab"},
            {"building_blocks", "purpur_slab", "hushstone_brick_wall"},
            {"building_blocks", "purpur_slab", "chiseled_hushstone_bricks"},
            {"building_blocks", "purpur_slab", "choirstone"},
            {"building_blocks", "purpur_slab", "polished_choirstone"},
            {"building_blocks", "purpur_slab", "polished_choirstone_stairs"},
            {"building_blocks", "purpur_slab", "polished_choirstone_slab"},
            {"building_blocks", "purpur_slab", "choirstone_bricks"},
            {"building_blocks", "purpur_slab", "cracked_choirstone_bricks"},
            {"building_blocks", "purpur_slab", "choirstone_brick_stairs"},
            {"building_blocks", "purpur_slab", "choirstone_brick_slab"},
            {"building_blocks", "purpur_slab", "choirstone_brick_wall"},
            {"building_blocks", "purpur_slab", "chiseled_choirstone"},
            {"building_blocks", "purpur_slab", "choirstone_tiles"},
            {"building_blocks", "purpur_slab", "choirstone_tile_stairs"},
            {"building_blocks", "purpur_slab", "choirstone_tile_slab"},
            {"building_blocks", "purpur_slab", "choirstone_pillar"},
            {"building_blocks", "purpur_slab", "stave_stone"},
            {"building_blocks", "purpur_slab", "dim_stave_stone"},
            {"building_blocks", "netherite_block", "resonite_block"},
            {"building_blocks", "netherite_block", "resonite_grate"},

            // Colored blocks: Aurelith's smoked nightglass beside tinted
            // glass, which this tab and the functional one both list.
            {"colored_blocks", "tinted_glass", "nightglass"},

            // Natural blocks: the Hush's ground after the End's end stone,
            // its ores after the Nether's, its crystals after amethyst, and
            // its log / leaves / sapling / grass / flower / fruit each after
            // the last vanilla row of the same kind.
            {"natural_blocks", "end_stone", "hushstone"},
            {"natural_blocks", "end_stone", "hush_moss"},
            {"natural_blocks", "end_stone", "sculk_loam"},
            {"natural_blocks", "ancient_debris", "echo_ore"},
            {"natural_blocks", "ancient_debris", "resonite_ore"},
            {"natural_blocks", "amethyst_cluster", "resonant_crystal"},
            {"natural_blocks", "amethyst_cluster", "resonant_cluster"},
            {"natural_blocks", "warped_stem", "whisperwood_log"},
            {"natural_blocks", "flowering_azalea_leaves", "lantern_leaves"},
            {"natural_blocks", "warped_fungus", "whisperwood_sapling"},
            {"natural_blocks", "dead_bush", "hush_grass"},
            {"natural_blocks", "wither_rose", "resonance_bloom"},
            {"natural_blocks", "sweet_berries", "whisperfruit"},

            // Functional blocks: light sources with their kin (the blue torch
            // after the redstone torch, lanterns after the copper lanterns,
            // the conduit chain after the copper chains, the display block
            // after the redstone lamp, Aurelith's lumen lights after the
            // froglights), the beam and aura blocks after the beacon and the
            // conduit, nightglass after tinted glass, and the Choir Hall /
            // Aurelith puzzle blocks after the vault.
            {"functional_blocks", "redstone_torch", "blue_redstone_torch"},
            {"functional_blocks", "waxed_oxidized_copper_lantern", "echo_lantern"},
            {"functional_blocks", "waxed_oxidized_copper_lantern", "choir_lamp"},
            {"functional_blocks", "waxed_oxidized_copper_lantern", "dim_choir_lamp"},
            {"functional_blocks", "waxed_oxidized_copper_chain", "crystal_conduit"},
            {"functional_blocks", "redstone_lamp", "display_block"},
            {"functional_blocks", "pearlescent_froglight", "cyan_lumen_panel"},
            {"functional_blocks", "pearlescent_froglight", "dim_cyan_lumen_panel"},
            {"functional_blocks", "pearlescent_froglight", "violet_lumen_panel"},
            {"functional_blocks", "pearlescent_froglight", "dim_violet_lumen_panel"},
            {"functional_blocks", "pearlescent_froglight", "amber_lumen_panel"},
            {"functional_blocks", "pearlescent_froglight", "dim_amber_lumen_panel"},
            {"functional_blocks", "pearlescent_froglight", "lumen_strip"},
            {"functional_blocks", "pearlescent_froglight", "dim_lumen_strip"},
            {"functional_blocks", "pearlescent_froglight", "guttering_amber_window"},
            {"functional_blocks", "pearlescent_froglight", "waking_amber_window"},
            {"functional_blocks", "pearlescent_froglight", "restless_amber_window"},
            {"functional_blocks", "pearlescent_froglight", "guttering_cyan_window"},
            {"functional_blocks", "pearlescent_froglight", "waking_cyan_window"},
            {"functional_blocks", "pearlescent_froglight", "guttering_violet_window"},
            {"functional_blocks", "pearlescent_froglight", "waking_violet_window"},
            {"functional_blocks", "beacon", "hush_lighthouse_lamp"},
            {"functional_blocks", "beacon", "resonance_engine"},
            {"functional_blocks", "beacon", "voice_beacon"},
            {"functional_blocks", "conduit", "echo_heart"},
            {"functional_blocks", "tinted_glass", "nightglass"},
            {"functional_blocks", "vault", "choir_altar"},
            {"functional_blocks", "vault", "resonant_chime"},
            {"functional_blocks", "vault", "chord_socket"},
            {"functional_blocks", "vault", "voice_pedestal"},
            {"functional_blocks", "vault", "choir_cabinet"},

            // Redstone blocks: the redstone_plus components beside their
            // vanilla counterparts.
            {"redstone_blocks", "redstone_torch", "blue_redstone_torch"},
            {"redstone_blocks", "redstone_lamp", "display_block"},

            // Tools & utilities: the resonite tier after netherite; the
            // tuning fork with the hand tools; the echo compass and recall
            // chime with the compasses; the portal gun and wand with the
            // ender pearl and eye; the cloak of silence with the elytra.
            {"tools_and_utilities", "netherite_hoe", "resonite_shovel"},
            {"tools_and_utilities", "netherite_hoe", "resonite_pickaxe"},
            {"tools_and_utilities", "netherite_hoe", "resonite_axe"},
            {"tools_and_utilities", "netherite_hoe", "resonite_hoe"},
            {"tools_and_utilities", "brush", "tuning_fork"},
            {"tools_and_utilities", "recovery_compass", "echo_compass"},
            {"tools_and_utilities", "recovery_compass", "recall_chime"},
            {"tools_and_utilities", "ender_eye", "portal_gun"},
            {"tools_and_utilities", "ender_eye", "portal_wand"},
            {"tools_and_utilities", "elytra", "cloak_of_silence"},

            // Combat: resonite sword and axe after netherite, the echo blade
            // (the Hush's capstone sword) after the resonite sword, the
            // resonance bow after the crossbow.
            {"combat", "netherite_sword", "resonite_sword"},
            {"combat", "netherite_sword", "echo_blade"},
            {"combat", "netherite_axe", "resonite_axe"},
            {"combat", "crossbow", "resonance_bow"},

            // Food & drinks: whisperfruit with the other berries.
            {"food_and_drinks", "glow_berries", "whisperfruit"},

            // Ingredients: raw resonite and its ingot in the ore rows, the
            // bosses' hearts after the nether star, the Aurelith voice keys
            // and held note after the trial keys.
            {"ingredients", "raw_gold", "raw_resonite"},
            {"ingredients", "netherite_ingot", "resonite_ingot"},
            {"ingredients", "nether_star", "resonant_heart"},
            {"ingredients", "nether_star", "choir_heart"},
            {"ingredients", "ominous_trial_key", "soprano_voice_key"},
            {"ingredients", "ominous_trial_key", "alto_voice_key"},
            {"ingredients", "ominous_trial_key", "tenor_voice_key"},
            {"ingredients", "ominous_trial_key", "bass_voice_key"},
            {"ingredients", "ominous_trial_key", "held_note"},

            // Spawn eggs: the echo core (it wakes the Silent Warden) with the
            // other mob-spawning blocks; the Hush's mobs as a dimension group
            // after the End's, alphabetical.
            {"spawn_eggs", "creaking_heart", "echo_core"},
            {"spawn_eggs", "shulker_spawn_egg", "choir_mother_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "crystal_golem_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "echo_mimic_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "echo_wraith_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "hush_leviathan_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "hushling_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "lumen_moth_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "silent_warden_spawn_egg"},
            {"spawn_eggs", "shulker_spawn_egg", "the_unsung_spawn_egg"},

            // Operator utilities: the occlusion wand edits world state for
            // every player — a debug-stick-class tool.
            {"op_blocks", "debug_stick", "ao_wand"},

            // Twilight Forest: the engine's egg for the upper goblin knight
            // (TF gives it none) beside the lower knight's.
            {"twilightforest", "lower_goblin_knight_spawn_egg", "upper_goblin_knight_spawn_egg"},

            // ── Deliberate deviations from MC (the user's call) ────────────
            // Vanilla items MC keeps out of creative, offered here where their
            // kin sit: the boss eggs in alphabetical position within their
            // group (the wither's before the wither skeleton's in the Nether
            // group, the dragon's before the enderman's in the End group), the
            // written and knowledge books after the book and quill, the
            // petrified oak slab after the oak slab.
            {"spawn_eggs", "strider_spawn_egg", "wither_spawn_egg"},
            {"spawn_eggs", "zombified_piglin_spawn_egg", "ender_dragon_spawn_egg"},
            {"tools_and_utilities", "writable_book", "written_book"},
            {"tools_and_utilities", "writable_book", "knowledge_book"},
            {"building_blocks", "oak_slab", "petrified_oak_slab"},
            // Twilight Forest blocks TF's own tabs leave out: the cinder wood
            // after the last TF wood set, the hardened dark leaves after the
            // dark leaves.
            {"twilightforest", "sorting_sapling", "cinder_log"},
            {"twilightforest", "sorting_sapling", "cinder_wood"},
            {"twilightforest", "dark_leaves", "hardened_dark_leaves"},
        };

        // Engine blocks with no item form (technical, legacy, or the block
        // half of an item that places it) — the engine's BlockIds.
        const char* const kEngineBlockOnly[] = {
            "chain",                      // legacy id of iron_chain
            "ominous_banner",             // unused model block; MC's is a banner component
            "set_spawn",                  // "Respawn point set" message block
            "blue_redstone_wall_torch",   // placed form of blue_redstone_torch
            "hush_portal",
            "resonant_water",             // a fluid, like water
            "hanging_whisperfruit",       // placed form of whisperfruit
        };

        // Custom items outside the pure-item table: registry path -> id.
        struct CustomItem { const char* path; const Game::ItemID* id; };

        std::vector<CustomItem> CustomItems() {
            std::vector<CustomItem> out;
#if ENABLE_PORTAL_GUN
            out.push_back({"portal_gun", &Game::Items::PortalGun});
#endif
#if ENABLE_IMMERSIVE_PORTALS
            out.push_back({"portal_wand", &Game::Items::PortalWand});
#endif
            out.push_back({"ao_wand", &Game::Items::AoWand});
            return out;
        }

        // ── Name index ─────────────────────────────────────────────────────
        struct NameIndex {
            std::unordered_map<std::string, Game::ItemID> byName;
            std::unordered_map<Game::ItemID, std::string> byId;
            std::unordered_set<std::string> vanillaItems, vanillaBlockOnly, aether, twilight, engineHidden;
            std::unordered_map<std::string, int> vanillaOrder;   // Items.java index
        };

        const NameIndex& Index() {
            static const NameIndex index = [] {
                NameIndex idx;
                // Block items first, then pure items over them: a pure item
                // owns its slug where a block shares it (the wheat crop vs
                // the wheat item), the rule the save format's name index uses.
                for (size_t i = 1; i < static_cast<size_t>(Game::BlockID::Count); ++i) {
                    const auto id = static_cast<Game::BlockID>(i);
                    const std::string& slug = Game::BlockRegistry::Get(id).registrySlug;
                    if (!slug.empty()) idx.byName.emplace(slug, Game::ItemRegistry::FromBlock(id));
                }
                const auto& pure = Game::ItemRegistry::AllPureItems();
                for (size_t i = 0; i < Game::kPureItemTableSize; ++i) {
                    const char* slug = Game::kPureItemTable[i].slug;
                    const auto id = static_cast<Game::ItemID>(Game::PURE_ITEM_BASE + i);
                    if (slug && pure.count(id)) idx.byName[slug] = id;
                }
                for (const CustomItem& c : CustomItems()) {
                    if (*c.id != Game::Items::Air && pure.count(*c.id)) idx.byName[c.path] = *c.id;
                }
                for (const auto& [name, id] : idx.byName) idx.byId[id] = name;

                int order = 0;
                for (const char* s : kVanillaItemIds) {
                    idx.vanillaItems.insert(s);
                    idx.vanillaOrder.emplace(s, order++);
                }
                for (const char* s : kVanillaBlockOnlyIds) idx.vanillaBlockOnly.insert(s);
                for (const char* s : kAetherItemIds) idx.aether.insert(s);
                for (const char* s : kTwilightItemIds) idx.twilight.insert(s);
                for (const char* s : kEngineBlockOnly) idx.engineHidden.insert(s);
                return idx;
            }();
            return index;
        }

        // ── ItemStackLinkedSet (type + components) ───────────────────────
        class LinkedStackSet {
        public:
            bool Contains(const Game::ItemStack& s) const {
                auto it = m_byItem.find(s.itemId);
                if (it == m_byItem.end()) return false;
                for (size_t i : it->second) {
                    if (Game::IsSameItemSameComponents(m_stacks[i], s)) return true;
                }
                return false;
            }
            bool Add(const Game::ItemStack& s) {
                if (Contains(s)) return false;
                m_byItem[s.itemId].push_back(m_stacks.size());
                m_stacks.push_back(s);
                return true;
            }
            std::vector<Game::ItemStack>& Stacks() { return m_stacks; }

        private:
            std::vector<Game::ItemStack> m_stacks;
            std::unordered_map<Game::ItemID, std::vector<size_t>> m_byItem;
        };

        // MC CreativeModeTab.ItemDisplayBuilder.
        class DisplayBuilder {
        public:
            DisplayBuilder(const std::string& tabKey, const CreativeModeTabs::DisplayParameters& params)
                : m_tabKey(tabKey), m_params(params) {}

            void Accept(const Game::ItemStack& stack, Visibility visibility) {
                if (stack.IsEmpty()) return;
                // MC throws "Accidentally adding the same item stack twice";
                // a repeat here (an engine row naming a stack the tab already
                // holds) is dropped instead, and said once.
                if (visibility != Visibility::SearchOnly && m_tab.Contains(stack)) {
                    Log::Warning("[CreativeTabs] %s: duplicate stack %s skipped", m_tabKey.c_str(),
                                 CreativeModeTabs::NameOf(stack.itemId).c_str());
                    return;
                }
                if (!IsEnabled(stack.itemId)) return;
                switch (visibility) {
                    case Visibility::ParentAndSearch: m_tab.Add(stack); m_search.Add(stack); break;
                    case Visibility::ParentOnly:      m_tab.Add(stack); break;
                    case Visibility::SearchOnly:      m_search.Add(stack); break;
                }
            }

            void Accept(Game::ItemID id, Visibility visibility) {
                if (id == Game::Items::Air) return;
                Accept(Game::ItemStack(id, 1), visibility);
            }

            std::vector<Game::ItemStack> TakeTab()    { return std::move(m_tab.Stacks()); }
            std::vector<Game::ItemStack> TakeSearch() { return std::move(m_search.Stacks()); }

        private:
            // MC Item.isEnabled(FeatureFlagSet): the engine's rule-gated
            // items — the redstone_plus blocks and the immersive-portal wand.
            // The portal gun is always offered (the user's call).
            bool IsEnabled(Game::ItemID id) const {
                if (id == Game::ItemRegistry::FromBlock(Game::BlockID::BlueRedstoneTorch) ||
                    id == Game::ItemRegistry::FromBlock(Game::BlockID::DisplayBlock)) {
                    return m_params.redstonePlus;
                }
#if ENABLE_IMMERSIVE_PORTALS
                if (id == Game::Items::PortalWand) return m_params.immersivePortals;
#endif
                return true;
            }

            std::string m_tabKey;
            const CreativeModeTabs::DisplayParameters& m_params;
            LinkedStackSet m_tab;
            LinkedStackSet m_search;
        };

        // ── Generators (MC CreativeModeTabs.generate*) ─────────────────────

        // Enchantments in registry order: a data-driven registry, loaded
        // sorted by id.
        std::vector<Game::EnchantmentId> EnchantmentsInRegistryOrder() {
            const auto& all = Game::EnchantmentRegistry::All();
            std::vector<Game::EnchantmentId> ids(all.size());
            for (size_t i = 0; i < all.size(); ++i) ids[i] = static_cast<Game::EnchantmentId>(i);
            std::stable_sort(ids.begin(), ids.end(), [&](Game::EnchantmentId a, Game::EnchantmentId b) {
                return all[a].slug < all[b].slug;
            });
            return ids;
        }

        void GeneratePaintings(DisplayBuilder& out, Game::ItemID painting, bool placeable, Visibility vis) {
            // PaintingVariants::All() is sorted by id — the registry order —
            // and MC sorts that stably by area, then width.
            const auto& all = Game::PaintingVariants::All();
            const auto& tagged = Game::PaintingVariants::Placeable();
            const std::unordered_set<int> placeableSet(tagged.begin(), tagged.end());
            std::vector<int> picked;
            for (int i = 0; i < static_cast<int>(all.size()); ++i) {
                if ((placeableSet.count(i) != 0) == placeable) picked.push_back(i);
            }
            std::stable_sort(picked.begin(), picked.end(), [&](int a, int b) {
                if (all[a].Area() != all[b].Area()) return all[a].Area() < all[b].Area();
                return all[a].width < all[b].width;
            });
            for (int index : picked) {
                Game::ItemStack stack(painting, 1);
                stack.components.set(Game::DataComponents::PAINTING_VARIANT, all[index].id);
                out.Accept(stack, vis);
            }
        }

        void GenerateSuspiciousStews(DisplayBuilder& out, Game::ItemID stew, Visibility vis) {
            // SuspiciousEffectHolder.getAllEffectHolders walks the ITEM
            // registry, so the flowers come in Items.java order; the
            // ItemStackLinkedSet folds flowers with equal effects into one.
            const auto& idx = Index();
            std::vector<Game::BlockID> flowers = Game::GetSuspiciousEffectFlowers();
            auto orderOf = [&](Game::BlockID b) {
                auto it = idx.vanillaOrder.find(Game::BlockRegistry::Get(b).registrySlug);
                return it != idx.vanillaOrder.end() ? it->second : INT32_MAX;
            };
            std::stable_sort(flowers.begin(), flowers.end(),
                             [&](Game::BlockID a, Game::BlockID b) { return orderOf(a) < orderOf(b); });
            LinkedStackSet stews;
            for (Game::BlockID flower : flowers) {
                const Game::SuspiciousStewEffects* effects = Game::GetFlowerSuspiciousEffects(flower);
                if (!effects) continue;
                Game::ItemStack stack(stew, 1);
                stack.components.set(Game::DataComponents::SUSPICIOUS_STEW_EFFECTS, *effects);
                stews.Add(stack);
            }
            for (const auto& s : stews.Stacks()) out.Accept(s, vis);
        }

        void GenerateEnchantedBooks(DisplayBuilder& out, bool allLevels, Visibility vis) {
            const auto& all = Game::EnchantmentRegistry::All();
            for (Game::EnchantmentId id : EnchantmentsInRegistryOrder()) {
                const auto& e = all[id];
                if (allLevels) {
                    for (int level = e.minLevel; level <= e.maxLevel; ++level) {
                        out.Accept(Game::EnchantmentHelper::CreateBook({id, level}), vis);
                    }
                } else {
                    out.Accept(Game::EnchantmentHelper::CreateBook({id, e.maxLevel}), vis);
                }
            }
        }

        // "slug:level,slug:level" -> ENCHANTMENTS (TF generateGearWithEnchants).
        Game::ItemStack EnchantedStack(Game::ItemID id, const char* spec) {
            Game::ItemStack stack(id, 1);
            if (!spec || !*spec) return stack;
            Game::ItemEnchantments enchantments;
            std::string_view rest(spec);
            while (!rest.empty()) {
                const size_t comma = rest.find(',');
                const std::string_view part = rest.substr(0, comma);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                const size_t colon = part.find(':');
                if (colon == std::string_view::npos) continue;
                const auto enchantment = Game::EnchantmentRegistry::ByName(part.substr(0, colon));
                if (!enchantment) continue;
                enchantments.Set(*enchantment, std::atoi(std::string(part.substr(colon + 1)).c_str()));
            }
            if (!enchantments.IsEmpty()) stack.components.set(Game::DataComponents::ENCHANTMENTS, enchantments);
            return stack;
        }

        void Expand(DisplayBuilder& out, const CreativeEntry& e) {
            if (!e.id) return;
            const Game::ItemID id = CreativeModeTabs::ItemByName(e.id);
            if (id == Game::Items::Air) return;   // not registered (yet)
            switch (e.kind) {
                case EntryKind::Item:
                    out.Accept(id, e.visibility);
                    break;
                case EntryKind::EnchantedItem:
                    out.Accept(EnchantedStack(id, e.extra), e.visibility);
                    break;
                case EntryKind::PresetPaintings:
                    GeneratePaintings(out, id, true, e.visibility);
                    break;
                case EntryKind::OpPaintings:
                    GeneratePaintings(out, id, false, e.visibility);
                    break;
                case EntryKind::Potions:
                    for (int p = 0; p < Game::kPotionCount; ++p) {
                        out.Accept(Game::CreatePotionItemStack(id, static_cast<Game::PotionId>(p)), e.visibility);
                    }
                    break;
                case EntryKind::SuspiciousStews:
                    GenerateSuspiciousStews(out, id, e.visibility);
                    break;
                case EntryKind::EnchantedBooksMax:
                    GenerateEnchantedBooks(out, false, e.visibility);
                    break;
                case EntryKind::EnchantedBooksAll:
                    GenerateEnchantedBooks(out, true, e.visibility);
                    break;
                // MC generateFireworksAllDurations: a rocket of each
                // craftable flight duration (1, 2, 3), no stars.
                case EntryKind::Fireworks:
                    for (const int duration : Game::FireworkItems::kCraftableDurations) {
                        Game::ItemStack rocket(id, 1);
                        Game::Fireworks fireworks;
                        fireworks.flightDuration = duration;
                        Game::FireworkItems::SetFireworks(rocket, fireworks);
                        out.Accept(rocket, e.visibility);
                    }
                    break;
                // MC generateOminousBottles: a bottle of each Bad Omen level,
                // amplifier 0..4 (0 is the bottle's own default — no patch).
                case EntryKind::OminousBottles:
                    for (int amplifier = 0; amplifier <= Game::DataComponents::kOminousBottleMaxAmplifier;
                         ++amplifier) {
                        Game::ItemStack bottle(id, 1);
                        if (amplifier != 0) {
                            bottle.components.set(Game::DataComponents::OMINOUS_BOTTLE_AMPLIFIER, amplifier);
                        }
                        out.Accept(bottle, e.visibility);
                    }
                    break;
                // MC generateInstrumentTypes: InstrumentItem.create(item, holder)
                // for every instrument in the tag (#goat_horns — the eight
                // horns; the ponder horn is the item's default, no patch).
                case EntryKind::Instruments: {
                    const std::string tag = std::string("#minecraft:") + (e.extra ? e.extra : "goat_horns");
                    const std::vector<std::string> instruments = Game::Instruments::Resolve(tag);
                    if (instruments.empty()) {
                        out.Accept(id, e.visibility);
                        break;
                    }
                    for (const std::string& instrument : instruments) {
                        Game::ItemStack horn(id, 1);
                        if (instrument != Game::Instruments::kDefaultGoatHorn) {
                            horn.components.set(Game::DataComponents::INSTRUMENT, instrument);
                        }
                        out.Accept(horn, e.visibility);
                    }
                    break;
                }
                // The engine has no BLOCK_STATE / TEST_BLOCK_MODE component, so
                // each generator yields the one variant a plain stack IS in
                // MC: the default test block and the level-15 light.
                case EntryKind::TestBlockModes:
                case EntryKind::LightLevels:
                    out.Accept(id, e.visibility);
                    break;
                // MC Raid.getOminousBannerInstance — the white banner with the
                // illager pattern layers (BANNER_PATTERNS), item_name and
                // UNCOMMON rarity.
                case EntryKind::OminousBanner:
                    out.Accept(Game::Raid::GetOminousBannerInstance(), e.visibility);
                    break;
                // The Aether's swet banner (AetherItems.createSwetBannerItemStack)
                // is not generated yet; a plain black banner is in the tab.
                case EntryKind::SwetBanner:
                    break;
            }
        }

        // ── Tab assembly ───────────────────────────────────────────────────

        struct TabSource {
            std::string key;
            std::string titleKey;
            std::string titleFallback;
            std::vector<const char*> iconChain;   // first registered id wins
            Row row = Row::Top;
            int column = 0;
            int page = 0;
            TabType type = TabType::Category;
            bool alignedRight = false, showTitle = true, canScroll = true, requiresPermissions = false;
            std::string background = "items";
            const CreativeEntry* entries = nullptr;
            size_t entryCount = 0;
        };

        // One pending insertion after an anchor.
        struct Insertion { const char* item; Visibility visibility; };

        std::vector<CreativeModeTab> g_tabs;
        std::optional<CreativeModeTabs::DisplayParameters> g_cached;

        std::vector<TabSource> TabSources() {
            std::vector<TabSource> out;
            for (const VanillaTabDef& def : kVanillaTabs) {
                TabSource s;
                s.key = def.key;
                s.titleKey = def.titleKey;
                s.titleFallback = def.titleKey;
                s.iconChain = {def.icon};
                s.row = def.row;
                s.column = def.column;
                s.type = def.type;
                s.alignedRight = def.alignedRight;
                s.showTitle = def.showTitle;
                s.canScroll = def.canScroll;
                s.requiresPermissions = def.requiresPermissions;
                s.background = def.background;
                s.entries = def.entries;
                s.entryCount = def.entryCount;
                out.push_back(std::move(s));
            }
            // Mod tabs, NeoForge-paged: page 1, the top row from the left.
            // The Aether's own tabs (building, dungeon, natural, functional,
            // redstone, equipment, armour, food, ingredients, spawn eggs) in
            // one tab, glowstone as its icon.
            TabSource aether;
            aether.key = "aether";
            aether.titleKey = "itemGroup.aether";
            aether.titleFallback = "The Aether";
            aether.iconChain = {"glowstone"};
            aether.row = Row::Top;
            aether.column = 0;
            aether.page = 1;
            aether.entries = kAetherTabEntries;
            aether.entryCount = std::size(kAetherTabEntries);
            out.push_back(std::move(aether));
            // Twilight Forest's blocks, items, equipment and food tabs in one;
            // TF's own tab icons in its order (the miniature structures the
            // engine lacks fall through to the equipment tab's pickaxe).
            TabSource twilight;
            twilight.key = "twilightforest";
            twilight.titleKey = "itemGroup.twilightforest";
            twilight.titleFallback = "Twilight Forest";
            twilight.iconChain = {"naga_courtyard_miniature_structure", "twilight_portal_miniature_structure",
                                  "knightmetal_pickaxe", "cooked_meef", "twilight_oak_sapling"};
            twilight.row = Row::Top;
            twilight.column = 1;
            twilight.page = 1;
            twilight.entries = kTwilightTabEntries;
            twilight.entryCount = std::size(kTwilightTabEntries);
            out.push_back(std::move(twilight));
            return out;
        }

        // Insertions per (tab, anchor): the engine table in its order, then
        // the mods' own insertAfter rows (PARENT_TAB_ONLY: a mod's items reach
        // the search tab through the mod's tab, grouped at the bottom).
        // NeoForge's insertAfter puts each row right after the anchor, so a
        // later row for the same anchor lands before an earlier one.
        std::unordered_map<std::string, std::vector<Insertion>> InsertionsFor(const std::string& tab) {
            std::unordered_map<std::string, std::vector<Insertion>> out;
            for (const Placement& p : kEnginePlacements) {
                if (tab == p.tab) out[p.after].push_back({p.item, Visibility::ParentAndSearch});
            }
            std::unordered_map<std::string, std::vector<Insertion>> mods;
            auto addMod = [&](const ModInsert* rows, size_t count) {
                for (size_t i = 0; i < count; ++i) {
                    if (rows[i].tab && tab == rows[i].tab) {
                        mods[rows[i].after].push_back({rows[i].item, Visibility::ParentOnly});
                    }
                }
            };
            addMod(kAetherInserts, kAetherInsertCount);
            addMod(kTwilightInserts, kTwilightInsertCount);
            for (auto& [anchor, list] : mods) {
                auto& dst = out[anchor];
                dst.insert(dst.end(), list.rbegin(), list.rend());
            }
            return out;
        }

        void BuildTab(CreativeModeTab& tab, const TabSource& src,
                      const CreativeModeTabs::DisplayParameters& params) {
            DisplayBuilder out(src.key, params);
            if (!src.requiresPermissions || params.hasPermissions) {
                auto insertions = InsertionsFor(src.key);
                std::unordered_set<std::string> anchorsSeen;
                for (size_t i = 0; i < src.entryCount; ++i) {
                    const CreativeEntry& e = src.entries[i];
                    Expand(out, e);
                    if (!e.id || e.kind != EntryKind::Item || anchorsSeen.count(e.id)) continue;
                    auto it = insertions.find(e.id);
                    if (it == insertions.end()) continue;
                    anchorsSeen.insert(e.id);
                    for (const Insertion& ins : it->second) {
                        out.Accept(CreativeModeTabs::ItemByName(ins.item), ins.visibility);
                    }
                    insertions.erase(it);
                }
                // An anchor the tab never names: its rows go at the end.
                for (const auto& [anchor, list] : insertions) {
                    static std::unordered_set<std::string> warned;
                    if (warned.insert(src.key + "|" + anchor).second) {
                        Log::Warning("[CreativeTabs] %s: anchor '%s' not in the tab; its rows go last",
                                     src.key.c_str(), anchor.c_str());
                    }
                    for (const Insertion& ins : list) {
                        out.Accept(CreativeModeTabs::ItemByName(ins.item), ins.visibility);
                    }
                }
            }
            tab.displayItems = out.TakeTab();
            tab.searchTabDisplayItems = out.TakeSearch();
        }

        // Engine-only items no table places (content added after the
        // placement table was written): offered at the end of the vanilla
        // search results so nothing registered is unreachable, and named once
        // in the log so it gets a proper place.
        std::vector<Game::ItemStack> UnplacedEngineItems(const std::vector<CreativeModeTab>& tabs,
                                                         const CreativeModeTabs::DisplayParameters& params) {
            const NameIndex& idx = Index();
            std::unordered_set<Game::ItemID> placed;
            for (const Placement& p : kEnginePlacements) placed.insert(CreativeModeTabs::ItemByName(p.item));
            for (const auto& tab : tabs) {
                for (const auto& s : tab.searchTabDisplayItems) placed.insert(s.itemId);
                for (const auto& s : tab.displayItems) placed.insert(s.itemId);
            }
            std::vector<std::pair<std::string, Game::ItemID>> loose;
            for (const auto& [name, id] : idx.byName) {
                if (placed.count(id) || idx.vanillaItems.count(name) || idx.vanillaBlockOnly.count(name) ||
                    idx.aether.count(name) || idx.twilight.count(name) || idx.engineHidden.count(name)) {
                    continue;
                }
                if (idx.byId.at(id) != name) continue;   // a block slug a pure item owns
                loose.emplace_back(name, id);
            }
            // Registration order: blocks by BlockID, pure items by table index.
            std::sort(loose.begin(), loose.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            DisplayBuilder out("search", params);
            static bool logged = false;
            std::string names;
            for (const auto& [name, id] : loose) {
                out.Accept(id, Visibility::SearchOnly);
                if (!names.empty()) names += ", ";
                names += name;
            }
            if (!loose.empty() && !logged) {
                logged = true;
                Log::Warning("[CreativeTabs] engine items with no creative-tab placement (search only): %s",
                             names.c_str());
            }
            return out.TakeSearch();
        }

        void BuildAll(const CreativeModeTabs::DisplayParameters& params) {
            const std::vector<TabSource> sources = TabSources();
            g_tabs.clear();
            g_tabs.resize(sources.size());
            size_t searchIndex = SIZE_MAX;
            for (size_t i = 0; i < sources.size(); ++i) {
                const TabSource& src = sources[i];
                CreativeModeTab& tab = g_tabs[i];
                tab.key = src.key;
                tab.titleKey = src.titleKey;
                tab.titleFallback = src.titleFallback;
                tab.row = src.row;
                tab.column = src.column;
                tab.page = src.page;
                tab.type = src.type;
                tab.alignedRight = src.alignedRight;
                tab.showTitle = src.showTitle;
                tab.canScroll = src.canScroll;
                tab.onEveryPage = src.alignedRight;   // search, hotbar, op, inventory
                tab.backgroundTexture = std::string(kBackgroundDir) + src.background + ".png";
                tab.icon = Game::ItemStack{};
                for (const char* icon : src.iconChain) {
                    const Game::ItemID id = CreativeModeTabs::ItemByName(icon);
                    if (id != Game::Items::Air) { tab.icon = Game::ItemStack(id, 1); break; }
                }
                if (src.type == TabType::Search) searchIndex = i;
                // MC buildAllTabContents: every CATEGORY tab first.
                if (src.type == TabType::Category) BuildTab(tab, src, params);
            }
            // The search tab (MC: every non-search tab's search contents, in
            // registry order, one per stack) — vanilla tabs, then engine items
            // no tab places, then the mod tabs, so mod items sit at the bottom
            // grouped per mod.
            if (searchIndex != SIZE_MAX) {
                LinkedStackSet search;
                for (const auto& tab : g_tabs) {
                    if (tab.type == TabType::Search || tab.page != 0) continue;
                    for (const auto& s : tab.searchTabDisplayItems) search.Add(s);
                }
                for (const auto& s : UnplacedEngineItems(g_tabs, params)) search.Add(s);
                for (const auto& tab : g_tabs) {
                    if (tab.type == TabType::Search || tab.page == 0) continue;
                    for (const auto& s : tab.searchTabDisplayItems) search.Add(s);
                }
                g_tabs[searchIndex].displayItems = std::move(search.Stacks());
                g_tabs[searchIndex].searchTabDisplayItems.clear();
            }
        }

    } // namespace

    std::string CreativeModeTab::DisplayName() const {
        return Game::Language::GetOrDefault(titleKey, titleFallback);
    }

    bool CreativeModeTab::Contains(const Game::ItemStack& stack) const {
        for (const auto& s : searchTabDisplayItems) {
            if (s.itemId == stack.itemId && Game::IsSameItemSameComponents(s, stack)) return true;
        }
        return false;
    }

    namespace CreativeModeTabs {

        DisplayParameters CurrentParameters(bool playerIsCreative) {
            DisplayParameters p;
            // MC hasPermissions: player.canUseGameMasterBlocks() (creative +
            // operator) && the "Operator Items Tab" option. The integrated
            // server's host is the operator; a remote client has no
            // permission level to read, so it never sees the tab.
            const bool host = Server::g_integratedServer != nullptr && Server::g_integratedServer->IsRunning();
            p.hasPermissions = playerIsCreative && host && Platform::g_gameSettings.GetOperatorItemsTab();
            p.redstonePlus = Game::RedstonePlus::Enabled();
#if ENABLE_IMMERSIVE_PORTALS
            p.immersivePortals = Game::Portals::ImmersiveNetherPortals();
#endif
            return p;
        }

        bool TryRebuildTabContents(const DisplayParameters& parameters) {
            if (g_cached && *g_cached == parameters && !g_tabs.empty()) return false;
            g_cached = parameters;
            BuildAll(parameters);
            return true;
        }

        const std::vector<CreativeModeTab>& AllTabs() {
            if (g_tabs.empty()) TryRebuildTabContents(DisplayParameters{});
            return g_tabs;
        }

        std::vector<const CreativeModeTab*> Tabs() {
            std::vector<const CreativeModeTab*> out;
            for (const auto& tab : AllTabs()) {
                if (tab.ShouldDisplay()) out.push_back(&tab);
            }
            return out;
        }

        int IndexOf(const std::string& key) {
            const auto& tabs = AllTabs();
            for (size_t i = 0; i < tabs.size(); ++i) {
                if (tabs[i].key == key) return static_cast<int>(i);
            }
            return -1;
        }

        const CreativeModeTab& SearchTab() {
            return AllTabs()[static_cast<size_t>(std::max(0, IndexOf("search")))];
        }

        const CreativeModeTab& DefaultTab() {
            return AllTabs()[static_cast<size_t>(std::max(0, IndexOf("building_blocks")))];
        }

        int PageCount() {
            int pages = 1;
            for (const CreativeModeTab* tab : Tabs()) pages = std::max(pages, tab->page + 1);
            return pages;
        }

        Game::ItemID ItemByName(const std::string& path) {
            const auto& byName = Index().byName;
            auto it = byName.find(path);
            return it != byName.end() ? it->second : Game::Items::Air;
        }

        std::string NameOf(Game::ItemID id) {
            const auto& byId = Index().byId;
            auto it = byId.find(id);
            return it != byId.end() ? it->second : std::string{};
        }

        std::string NamespaceOf(Game::ItemID id) {
            const NameIndex& idx = Index();
            const std::string name = NameOf(id);
            if (name.empty() || idx.vanillaItems.count(name)) return "minecraft";
            if (idx.aether.count(name)) return "aether";
            if (idx.twilight.count(name)) return "twilightforest";
            for (const CustomItem& c : CustomItems()) {
                if (name == c.path) return "obeycraft";
            }
            return "minecraft";
        }

    } // namespace CreativeModeTabs

} // namespace Render
