// File: src/client/renderer/gui/CreativeModeTabs.hpp
//
// The creative inventory's tabs — mirrors net.minecraft.world.item.
// CreativeModeTab / CreativeModeTabs (26.3), plus the two mod tabs (the
// Aether, Twilight Forest) laid out on a second page the way NeoForge pages
// tabs past vanilla's 2 x 7.
//
// Tab contents are data (GeneratedCreativeModeTabs.inc, from
// tools/gen_creative_tabs.py) holding REGISTRY IDS, resolved against the item
// registry each time the tabs are built: an id the engine does not register
// is skipped, and one registered later appears in its MC position without
// touching this code. Engine-only content (the Hush, redstone_plus, the
// portal gun...) is slotted into the vanilla tabs by the placement table in
// CreativeModeTabs.cpp, where Mojang would have put it.
#pragma once

#include "common/entity/Item.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Render {

    // MC CreativeModeTab.
    class CreativeModeTab {
    public:
        enum class Row  : uint8_t { Top, Bottom };
        enum class Type : uint8_t { Category, Inventory, Hotbar, Search };

        std::string key;            // registry path: "building_blocks", "aether"
        std::string titleKey;       // "itemGroup.buildingBlocks"
        std::string titleFallback;  // shown when the language has no titleKey
        Row  row    = Row::Top;
        int  column = 0;            // 0..6; aligned-right tabs count from the right
        int  page   = 0;            // 0 = vanilla; mod tabs from 1 (NeoForge paging)
        Type type   = Type::Category;
        bool alignedRight = false;
        bool showTitle    = true;
        bool canScroll    = true;
        // Page-independent tabs (search, hotbar, op, inventory) sit on every page.
        bool onEveryPage  = false;
        std::string backgroundTexture;   // asset path of tab_<name>.png
        Game::ItemStack icon;

        // MC displayItems / displayItemsSearchTab (ItemStackLinkedSets:
        // one stack per item + components, in insertion order).
        std::vector<Game::ItemStack> displayItems;
        std::vector<Game::ItemStack> searchTabDisplayItems;

        // MC CreativeModeTab.getDisplayName().
        std::string DisplayName() const;
        // MC shouldDisplay: a category tab with nothing in it is hidden.
        bool ShouldDisplay() const { return type != Type::Category || !displayItems.empty(); }
        // MC contains(stack): the tab offers this exact stack to the search tab.
        bool Contains(const Game::ItemStack& stack) const;
    };

    namespace CreativeModeTabs {

        // MC CreativeModeTab.ItemDisplayParameters: everything the display
        // lists depend on. A change rebuilds every tab.
        struct DisplayParameters {
            bool hasPermissions   = false;   // op items tab (MC hasPermissions)
            bool redstonePlus     = false;   // redstone_plus rule: blue torch, display block
            bool immersivePortals = true;    // immersive_portals rule: the portal wand

            bool operator==(const DisplayParameters& o) const {
                return hasPermissions == o.hasPermissions && redstonePlus == o.redstonePlus &&
                       immersivePortals == o.immersivePortals;
            }
            bool operator!=(const DisplayParameters& o) const { return !(*this == o); }
        };

        // The parameters as the local client sees them right now.
        DisplayParameters CurrentParameters(bool playerIsCreative);

        // MC CreativeModeTabs.tryRebuildTabContents: rebuilds every tab when
        // the parameters differ from the last build (or nothing is built).
        // True when it rebuilt.
        bool TryRebuildTabContents(const DisplayParameters& parameters);

        // Every tab, registry order (vanilla, then the Aether, then Twilight
        // Forest). Valid after the first TryRebuildTabContents.
        const std::vector<CreativeModeTab>& AllTabs();
        // MC tabs(): the tabs that display.
        std::vector<const CreativeModeTab*> Tabs();
        const CreativeModeTab& SearchTab();
        // MC getDefaultTab: building blocks.
        const CreativeModeTab& DefaultTab();
        // Pages holding at least one displayed tab (1 with no mod tabs).
        int PageCount();
        // Index into AllTabs() of `key`, -1 when there is none.
        int IndexOf(const std::string& key);

        // Registry path -> ItemID (the ids tab rows name); Air when the
        // engine does not register it.
        Game::ItemID ItemByName(const std::string& path);
        // The id path of `id` ("oak_log", "portal_gun"); empty when unknown.
        std::string NameOf(Game::ItemID id);
        // The id namespace a creative search matches ("minecraft", "aether",
        // "twilightforest", or the engine's own for engine-only items).
        std::string NamespaceOf(Game::ItemID id);

    } // namespace CreativeModeTabs

} // namespace Render
