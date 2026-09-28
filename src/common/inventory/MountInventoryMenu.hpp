// File: src/common/inventory/MountInventoryMenu.hpp
//
// Mirrors net.minecraft.world.inventory.AbstractMountInventoryMenu with its
// two concretes, HorseInventoryMenu (horse, donkey, mule, skeleton / zombie
// horse, llama, trader llama, camel, camel husk) and NautilusInventoryMenu
// (nautilus, zombie nautilus). The two differ only in which equipment slots
// are active and in their empty-slot sprites, so one class carries both,
// keyed on Kind.
//
// Slot order — the wire's index space, MC's verbatim:
//   0                       the saddle            (ArmorSlot over SADDLE)
//   1                       the body armour       (ArmorSlot over BODY)
//   2 .. 2+cols*3-1         the chest, 3 rows of `inventoryColumns`
//   then                    player main inventory (3 rows x 9), hotbar
//
// The mount is never held by pointer. The menu is handed a resolver that
// looks it up by entity id each time it is needed, so a mount that dies,
// despawns or unloads while the screen is up leaves the menu reading empty
// slots rather than freed memory until the session's stillValid check
// closes it.
//
// SERVER: the equipment slots read and write the mob's own equipment (MC
// Mob.createEquipmentSlotContainer: a placed piece becomes a guaranteed drop
// and the mob persistent), the chest slots the mount's MountInventory.
// CLIENT: all three are scratch containers the server's slot sync fills
// (MC's client builds the horse menu over a fresh SimpleContainer the same
// way); the mob is only consulted for the slots' mayPlace / isActive.
#pragma once

#include "AbstractContainerMenu.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Game {

    class Mob;

    class MountInventoryMenu : public AbstractContainerMenu {
    public:
        // Looks the mount up afresh; null once it is gone.
        using MountResolver = std::function<Mob*()>;

        // HorseInventoryMenu or NautilusInventoryMenu.
        enum class Kind : uint8_t { Horse, Nautilus };
        // MC ClientPacketListener.handleMountScreenOpen's instanceof test:
        // an AbstractNautilus gets the nautilus menu, everything else the
        // horse one.
        static Kind KindFor(EntityTypeId type);

        // `serverSide` picks the backing containers (see the file comment).
        MountInventoryMenu(Inventory* playerInventory, MountResolver mount, Kind kind,
                           int inventoryColumns, bool serverSide);
        ~MountInventoryMenu() override;

        Kind GetKind() const { return m_kind; }
        int  InventoryColumns() const { return m_inventoryColumns; }
        int  MountContainerSize() const { return GetInventorySize(m_inventoryColumns); }
        // The mount, or null when it is gone.
        Mob* Mount() const { return m_mount ? m_mount() : nullptr; }

        // MC hasInventoryChanged(mountContainer) — the mount's inventory was
        // re-created (a chest put on or taken off) since the menu opened, or
        // the mount is gone. Server-side; the client menu never goes stale.
        bool HasInventoryChanged() const;

        // MC AbstractMountInventoryMenu.quickMoveStack.
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;

        // MC AbstractMountInventoryMenu's constants.
        static constexpr int SLOT_SADDLE          = 0;
        static constexpr int SLOT_BODY_ARMOR      = 1;
        static constexpr int SLOT_INVENTORY_START = 2;
        static constexpr int INVENTORY_ROWS       = 3;
        // MC AbstractMountInventoryMenu.getInventorySize.
        static int GetInventorySize(int inventoryColumns) { return inventoryColumns * INVENTORY_ROWS; }

    private:
        MountResolver m_mount;
        Kind          m_kind;
        int           m_inventoryColumns = 0;
        bool          m_serverSide = false;
        // The MountInventory generation the server menu opened over.
        uint32_t      m_inventoryGeneration = 0;
        // The saddle, body and chest containers (owned: views over the mob
        // on the server, scratch storage on the client).
        std::vector<std::unique_ptr<IContainer>> m_containers;
    };

} // namespace Game
