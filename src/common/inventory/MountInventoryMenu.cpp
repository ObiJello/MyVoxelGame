// File: src/common/inventory/MountInventoryMenu.cpp
//
// See MountInventoryMenu.hpp. MC sources: AbstractMountInventoryMenu,
// HorseInventoryMenu, NautilusInventoryMenu, ArmorSlot and
// Mob.createEquipmentSlotContainer.
#include "MountInventoryMenu.hpp"

#include "SimpleContainer.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/tags/DataTags.hpp"

#include <utility>

namespace Game {

    namespace {

        constexpr int SLOT_STEP = 18;

        bool IsLlamaType(EntityTypeId type) {
            return type == EntityTypeId::Llama || type == EntityTypeId::TraderLlama;
        }

        bool HasEntityTypeTag(const Mob& mob, const char* tag) {
            return DataTags::HasTag(DataTags::Registry::EntityType, mob.TypeInfo().slug, tag);
        }

        // MC Mob.createEquipmentSlotContainer(slot) — a one-slot container
        // over the mob's equipment (server). Reads come from the mob; a write
        // is MC's setTheItem: setItemSlot (onEquipItem's sound and game
        // event), and a non-empty piece becomes a guaranteed drop and the mob
        // persistent.
        //
        // The click code edits stacks in place through the reference GetItem
        // hands out and then calls SetChanged, so the container hands out a
        // view and commits it on SetChanged. A read while an in-place edit is
        // still uncommitted keeps the edit (the view differs from what was
        // last handed out) instead of reloading over it.
        class MountEquipmentContainer final : public IContainer {
        public:
            MountEquipmentContainer(MountInventoryMenu::MountResolver mount, EquipmentSlot slot)
                : m_mount(std::move(mount)), m_slot(slot) {}

            int GetContainerSize() const override { return 1; }
            int GetMaxStackSize() const override { return 1; }

            ItemStack& GetItem(int index) override {
                if (index != 0) { m_scratch = ItemStack{}; return m_scratch; }
                Refresh();
                return m_view;
            }
            const ItemStack& GetItem(int index) const override {
                if (index != 0) return m_scratch;
                Refresh();
                return m_view;
            }
            void SetItem(int index, const ItemStack& stack) override {
                if (index != 0) return;
                m_view = stack;
                Commit();
            }
            void SetChanged() override { Commit(); }

        private:
            static bool SameStack(const ItemStack& a, const ItemStack& b) {
                if (a.IsEmpty() || b.IsEmpty()) return a.IsEmpty() == b.IsEmpty();
                return a.count == b.count && IsSameItemSameComponents(a, b);
            }

            void Refresh() const {
                // An uncommitted in-place edit: keep it for the commit.
                if (!SameStack(m_view, m_handedOut)) return;
                Mob* mob = m_mount ? m_mount() : nullptr;
                m_view = mob ? mob->GetEquipment(m_slot) : ItemStack{};
                m_handedOut = m_view;
            }

            void Commit() {
                if (m_view.count <= 0) m_view = ItemStack{};
                m_handedOut = m_view;
                Mob* mob = m_mount ? m_mount() : nullptr;
                if (!mob) return;
                if (SameStack(mob->GetEquipment(m_slot), m_view)) return;
                // setTheItem: setItemSlot, then for a piece put on
                // setGuaranteedDrop + setPersistenceRequired.
                mob->SetEquipment(m_slot, m_view);
                if (!m_view.IsEmpty()) {
                    mob->SetGuaranteedDrop(m_slot);
                    mob->SetPersistenceRequired(true);
                }
            }

            MountInventoryMenu::MountResolver m_mount;
            EquipmentSlot m_slot;
            mutable ItemStack m_view{};
            mutable ItemStack m_handedOut{};
            ItemStack m_scratch{};
        };

        // The mount's chest (server): the MountInventory's container while it
        // is still the one the menu opened over; empty and inert once it was
        // re-created or the mount is gone (the session closes the menu on
        // the next stillValid check).
        class MountStorageContainer final : public IContainer {
        public:
            MountStorageContainer(MountInventoryMenu::MountResolver mount, uint32_t generation, int size)
                : m_mount(std::move(mount)), m_generation(generation), m_size(size) {}

            int GetContainerSize() const override { return m_size; }

            ItemStack& GetItem(int index) override {
                if (SimpleContainer* live = Live()) return live->GetItem(index);
                m_scratch = ItemStack{};
                return m_scratch;
            }
            const ItemStack& GetItem(int index) const override {
                if (const SimpleContainer* live = Live()) return live->GetItem(index);
                return kEmpty;
            }
            void SetItem(int index, const ItemStack& stack) override {
                if (SimpleContainer* live = Live()) live->SetItem(index, stack);
            }

        private:
            SimpleContainer* Live() const {
                Mob* mob = m_mount ? m_mount() : nullptr;
                MountInventory* inventory = mob ? mob->GetMountInventory() : nullptr;
                if (!inventory || inventory->Generation() != m_generation || inventory->Size() != m_size) {
                    return nullptr;
                }
                return &inventory->Container();
            }

            static inline const ItemStack kEmpty{};
            MountInventoryMenu::MountResolver m_mount;
            uint32_t  m_generation;
            int       m_size;
            ItemStack m_scratch{};
        };

        // MC ArmorSlot over a mount (HorseInventoryMenu / NautilusInventory-
        // Menu's anonymous subclasses): one piece, the mob's isEquippableInSlot
        // decides what fits, Curse of Binding holds it outside creative, and
        // isActive is the menu's rule for the slot.
        class MountEquipmentSlot final : public Slot {
        public:
            MountEquipmentSlot(IContainer* container, int x, int y, EquipmentSlot slot,
                               const MountInventoryMenu& menu, const char* emptyIcon)
                : Slot(container, 0, x, y), m_slot(slot), m_menu(menu) {
                noItemIcon = emptyIcon;
            }

            int GetMaxStackSize() const override { return 1; }

            // ArmorSlot.mayPlace: owner.isEquippableInSlot(stack, slot).
            bool MayPlace(const ItemStack& stack) const override {
                const Mob* mob = m_menu.Mount();
                return mob && mob->IsEquippableInSlot(stack, m_slot);
            }

            // ArmorSlot.mayPickup: a prevent_armor_change piece stays on
            // unless the player is creative.
            bool MayPickup() const override {
                const ItemStack& item = GetItem();
                if (!item.IsEmpty() && !m_menu.creative && EnchantmentHelper::HasPreventArmorChange(item)) {
                    return false;
                }
                return Slot::MayPickup();
            }

            bool IsActive() const override {
                const Mob* mob = m_menu.Mount();
                if (!mob || !mob->CanUseSlot(m_slot)) return false;
                if (m_menu.GetKind() == MountInventoryMenu::Kind::Nautilus) return true;
                // HorseInventoryMenu: the saddle slot for #can_equip_saddle,
                // the armour slot for #can_wear_horse_armor or a llama.
                if (m_slot == EquipmentSlot::SADDLE) return HasEntityTypeTag(*mob, "minecraft:can_equip_saddle");
                return HasEntityTypeTag(*mob, "minecraft:can_wear_horse_armor") || IsLlamaType(mob->GetType());
            }

        private:
            EquipmentSlot             m_slot;
            const MountInventoryMenu& m_menu;
        };

    } // namespace

    MountInventoryMenu::Kind MountInventoryMenu::KindFor(EntityTypeId type) {
        return type == EntityTypeId::Nautilus || type == EntityTypeId::ZombieNautilus ? Kind::Nautilus
                                                                                      : Kind::Horse;
    }

    MountInventoryMenu::MountInventoryMenu(Inventory* playerInventory, MountResolver mount, Kind kind,
                                           int inventoryColumns, bool serverSide)
        : AbstractContainerMenu(playerInventory), m_mount(std::move(mount)), m_kind(kind),
          m_inventoryColumns(inventoryColumns < 0 ? 0 : inventoryColumns), m_serverSide(serverSide) {
        const int storageSize = GetInventorySize(m_inventoryColumns);

        IContainer* saddleContainer = nullptr;
        IContainer* armorContainer  = nullptr;
        IContainer* storage         = nullptr;
        if (m_serverSide) {
            if (Mob* mob = Mount()) {
                if (const MountInventory* inventory = mob->GetMountInventory()) {
                    m_inventoryGeneration = inventory->Generation();
                }
            }
            m_containers.push_back(std::make_unique<MountEquipmentContainer>(m_mount, EquipmentSlot::SADDLE));
            saddleContainer = m_containers.back().get();
            m_containers.push_back(std::make_unique<MountEquipmentContainer>(m_mount, EquipmentSlot::BODY));
            armorContainer = m_containers.back().get();
            m_containers.push_back(std::make_unique<MountStorageContainer>(m_mount, m_inventoryGeneration,
                                                                           storageSize));
            storage = m_containers.back().get();
        } else {
            m_containers.push_back(std::make_unique<SimpleContainer>(1));
            saddleContainer = m_containers.back().get();
            m_containers.push_back(std::make_unique<SimpleContainer>(1));
            armorContainer = m_containers.back().get();
            m_containers.push_back(std::make_unique<SimpleContainer>(storageSize));
            storage = m_containers.back().get();
        }

        // The two ArmorSlots at (8, 18) and (8, 36) with the menu's sprites:
        // container/slot/saddle, and the armour slot's horse_armor /
        // llama_armor (HorseInventoryMenu) or nautilus_armor_inventory.
        const char* armorSprite = "container/slot/horse_armor";
        if (m_kind == Kind::Nautilus) {
            armorSprite = "container/slot/nautilus_armor_inventory";
        } else if (const Mob* mob = Mount(); mob && IsLlamaType(mob->GetType())) {
            armorSprite = "container/slot/llama_armor";
        }
        AddSlot(std::make_unique<MountEquipmentSlot>(saddleContainer, 8, 18, EquipmentSlot::SADDLE, *this,
                                                     "container/slot/saddle"));
        AddSlot(std::make_unique<MountEquipmentSlot>(armorContainer, 8, 36, EquipmentSlot::BODY, *this,
                                                     armorSprite));

        // HorseInventoryMenu: the chest grid at (80 + x*18, 18 + y*18).
        // NautilusInventoryMenu adds none (its columns are always 0).
        if (m_kind == Kind::Horse) {
            for (int y = 0; y < INVENTORY_ROWS; ++y) {
                for (int x = 0; x < m_inventoryColumns; ++x) {
                    AddSlot(std::make_unique<Slot>(storage, x + y * m_inventoryColumns,
                                                   80 + x * SLOT_STEP, 18 + y * SLOT_STEP));
                }
            }
        }

        // addStandardInventorySlots(playerInventory, 8, 84).
        for (int i = 0; i < 27; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::MAIN_BEGIN + i,
                                           8 + (i % 9) * SLOT_STEP, 84 + (i / 9) * SLOT_STEP));
        }
        for (int i = 0; i < 9; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::HOTBAR_BEGIN + i,
                                           8 + i * SLOT_STEP, 84 + 58));
        }
    }

    MountInventoryMenu::~MountInventoryMenu() = default;

    bool MountInventoryMenu::HasInventoryChanged() const {
        if (!m_serverSide) return false;
        const Mob* mob = Mount();
        const MountInventory* inventory = mob ? mob->GetMountInventory() : nullptr;
        return !inventory || inventory->Generation() != m_inventoryGeneration;
    }

    int MountInventoryMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        const int playerStart = SLOT_INVENTORY_START + (m_kind == Kind::Horse ? MountContainerSize() : 0);
        if (Inventory::IsMainSlot(inventoryIndex)) return playerStart + (inventoryIndex - Inventory::MAIN_BEGIN);
        if (Inventory::IsHotbarSlot(inventoryIndex)) return playerStart + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        return -1;
    }

    void MountInventoryMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        // AbstractMountInventoryMenu.quickMoveStack, verbatim.
        if (!IsValidSlotIndex(slotIndex)) return;
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;

        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const int mountSize = m_kind == Kind::Horse ? MountContainerSize() : 0;
        const int playerContainerStart = SLOT_INVENTORY_START + mountSize;
        const int total = SlotCount();

        const auto finish = [&] {
            // stack.isEmpty() ? slot.setByPlayer(EMPTY) : slot.setChanged().
            if (stack.IsEmpty()) slot.SetByPlayer(ItemStack{});
            else                 slot.SetChanged();
            if (stack.count != original.count || stack.IsEmpty() != original.IsEmpty()) {
                MarkChanged(result, slotIndex);
            }
        };

        if (slotIndex < playerContainerStart) {
            if (!MoveItemStackTo(stack, playerContainerStart, total, true, result)) return;
            finish();
            return;
        }
        Slot& armorSlot  = GetSlot(SLOT_BODY_ARMOR);
        Slot& saddleSlot = GetSlot(SLOT_SADDLE);
        if (armorSlot.MayPlace(stack) && !armorSlot.HasItem()) {
            if (!MoveItemStackTo(stack, SLOT_BODY_ARMOR, SLOT_BODY_ARMOR + 1, false, result)) return;
            finish();
            return;
        }
        if (saddleSlot.MayPlace(stack) && !saddleSlot.HasItem()) {
            if (!MoveItemStackTo(stack, SLOT_SADDLE, SLOT_SADDLE + 1, false, result)) return;
            finish();
            return;
        }
        if (mountSize == 0 || !MoveItemStackTo(stack, SLOT_INVENTORY_START, playerContainerStart, false, result)) {
            // Between the player's main rows and hotbar. MC returns EMPTY
            // here without the setChanged: the moved stack was edited in
            // place (a player slot), which is all that is needed.
            const int playerContainerEnd = playerContainerStart + 27;
            const int hotbarStart = playerContainerEnd;
            const int hotbarEnd = hotbarStart + 9;
            bool moved;
            if (slotIndex >= hotbarStart && slotIndex < hotbarEnd) {
                moved = MoveItemStackTo(stack, playerContainerStart, playerContainerEnd, false, result);
            } else if (slotIndex >= playerContainerStart && slotIndex < playerContainerEnd) {
                moved = MoveItemStackTo(stack, hotbarStart, hotbarEnd, false, result);
            } else {
                moved = MoveItemStackTo(stack, hotbarStart, playerContainerEnd, false, result);
            }
            if (moved) {
                slot.SetChanged();
                MarkChanged(result, slotIndex);
            }
            return;
        }
        finish();
    }

} // namespace Game
