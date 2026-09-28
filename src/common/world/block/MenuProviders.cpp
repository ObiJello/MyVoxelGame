// File: src/common/world/block/MenuProviders.cpp
#include "MenuProviders.hpp"

#include "BlockRegistry.hpp"
#include "common/world/portal/PortalState.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace Game {

    namespace {

        // Keyed by registry slug, the same names BlockBehaviors wires the
        // container uses onto — the menu type each block's provider builds.
        const std::unordered_map<std::string_view, MenuType>& ProviderTable() {
            static const std::unordered_map<std::string_view, MenuType> table = [] {
                std::unordered_map<std::string_view, MenuType> t;
                // ChestBlock / BarrelBlock / ShulkerBoxBlock: ChestMenu.threeRows
                // (the double chest's sixRows is decided when it opens).
                for (std::string_view slug : {
                         "chest", "trapped_chest", "barrel", "shulker_box",
                         "white_shulker_box", "orange_shulker_box", "magenta_shulker_box",
                         "light_blue_shulker_box", "yellow_shulker_box", "lime_shulker_box",
                         "pink_shulker_box", "gray_shulker_box", "light_gray_shulker_box",
                         "cyan_shulker_box", "purple_shulker_box", "blue_shulker_box",
                         "brown_shulker_box", "green_shulker_box", "red_shulker_box",
                         "black_shulker_box"}) {
                    t.emplace(slug, MenuType::Generic9x3);
                }
                t.emplace("dispenser",         MenuType::Generic3x3);
                t.emplace("dropper",           MenuType::Generic3x3);
                t.emplace("hopper",            MenuType::Hopper);
                t.emplace("furnace",           MenuType::Furnace);
                t.emplace("blast_furnace",     MenuType::BlastFurnace);
                t.emplace("smoker",            MenuType::Smoker);
                t.emplace("crafter",           MenuType::Crafter3x3);
                // The SimpleMenuProvider blocks.
                t.emplace("crafting_table",    MenuType::Crafting);
                t.emplace("stonecutter",       MenuType::Stonecutter);
                t.emplace("grindstone",        MenuType::Grindstone);
                t.emplace("cartography_table", MenuType::CartographyTable);
                t.emplace("loom",              MenuType::Loom);
                t.emplace("smithing_table",    MenuType::Smithing);
                t.emplace("anvil",             MenuType::Anvil);
                t.emplace("chipped_anvil",     MenuType::Anvil);
                t.emplace("damaged_anvil",     MenuType::Anvil);
                t.emplace("enchanting_table",  MenuType::Enchantment);
                t.emplace("brewing_stand",     MenuType::BrewingStand);
                t.emplace("beacon",            MenuType::Beacon);
                return t;
            }();
            return table;
        }

    } // namespace

    std::optional<MenuType> BlockMenuProviderType(BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return std::nullopt;
        const std::string& slug = BlockRegistry::Get(id).registrySlug;
        // MC LecternBlock.getMenuProvider: null unless HAS_BOOK.
        if (slug == "lectern") {
            if (state.GetValueByName("has_book") == "true") return MenuType::Lectern;
            return std::nullopt;
        }
        const auto& table = ProviderTable();
        const auto it = table.find(std::string_view(slug));
        if (it == table.end()) return std::nullopt;
        return it->second;
    }

    bool ShowAsInteractableInSpectatorMode(BlockState state) {
        return BlockMenuProviderType(state).has_value() || Portals::IsPortal(state.Block());
    }

} // namespace Game
