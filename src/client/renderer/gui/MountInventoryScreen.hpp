// File: src/client/renderer/gui/MountInventoryScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory
// .AbstractMountInventoryScreen with its two concretes, HorseInventoryScreen
// (textures/gui/container/horse.png, the container/horse/chest_slots grid)
// and NautilusInventoryScreen (textures/gui/container/nautilus.png, no
// chest) — one class keyed on the menu's Kind, as MountInventoryMenu is.
//
//   the panel        176 x 166, blitted whole from its 256 x 256 sheet
//   the chest grid   container/horse/chest_slots, the left `columns * 18`
//                    of its 90 x 54 at (79, 17), when the mount has one
//   saddle / armour  a container/slot frame at (7, 17) / (7, 35) while
//                    that slot is active (shouldRenderSaddleSlot /
//                    shouldRenderArmorSlot — the menu's ArmorSlot rules)
//   labels           the mount's name at (8, 6), "Inventory" at (8, 72)
//
// Opened by MountScreenOpenS2C (MC ClientPacketListener
// .handleMountScreenOpen): the menu is built over scratch containers and the
// server's slot snapshot fills it.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/inventory/MountInventoryMenu.hpp"

#include <cstdint>
#include <string>

namespace Render {

    class MountInventoryScreen : public AbstractContainerScreen {
    public:
        static constexpr int IMAGE_W = 176;
        static constexpr int IMAGE_H = 166;
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;   // -12566464

        void Configure(Game::MountInventoryMenu::Kind kind, int inventoryColumns, const std::string& title);

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;

    private:
        TextureHandle EnsureBackground();
        // shouldRenderSaddleSlot / shouldRenderArmorSlot: the menu's own
        // slot is active.
        bool SlotActive(int menuIndex) const;

        Game::MountInventoryMenu::Kind m_kind = Game::MountInventoryMenu::Kind::Horse;
        int           m_inventoryColumns = 0;
        std::string   m_title;
        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
        Game::MountInventoryMenu::Kind m_backgroundKind = Game::MountInventoryMenu::Kind::Horse;
    };

    MountInventoryScreen& GetMountInventoryScreen();

    // MC ClientPacketListener.handleMountScreenOpen: remember the mount and
    // its columns, then open MenuType::MountInventory through
    // OpenClientContainerScreen (which closes whatever screen is up and
    // calls BuildClientMountScreen). Nothing opens when this client does not
    // know the mount or it has no inventory screen (MC's instanceof tests).
    void OpenClientMountScreen(uint32_t containerId, int inventoryColumns, int32_t entityId);

    // OpenClientContainerScreen's MountInventory case: the menu over the
    // last requested mount (scratch containers, the server's snapshot fills
    // them), put on top, and this screen shown. False — and nothing built —
    // when no mount screen was requested or the mount is gone.
    bool BuildClientMountScreen(Game::Inventory* inventory, bool creative, uint32_t containerId,
                                const std::string& title);

} // namespace Render
