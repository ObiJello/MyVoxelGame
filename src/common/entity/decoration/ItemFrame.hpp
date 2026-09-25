// File: src/common/entity/decoration/ItemFrame.hpp
//
// MC net.minecraft.world.entity.decoration.ItemFrame and GlowItemFrame — a
// 12×12-pixel frame hung on any face of a block (floors and ceilings too),
// holding one item turned to one of eight rotations. The hanging half —
// cell, facing, survival, breaking — is HangingEntity's; the glow frame is
// the same entity with its own sounds, its own drop and (client side) a
// brighter frame and a full-bright item.
//
// WHAT IS HERE, IN MC'S TERMS
//   • interact: an empty frame takes one of the held item; a filled one
//     turns its item 45° (rotate sound).
//   • hurtServer: a hit on a filled frame knocks the item out (remove
//     sound); a hit on an empty one — or any explosion — breaks the frame.
//     A fixed frame takes nothing but the void and creative players.
//   • survives: its own box clear of blocks, the block BEHIND it isSolid
//     (a repeater / comparator also holds a wall frame), and no frame
//     facing the same way in its space.
//   • The comparator reading (getAnalogOutput): 0 when empty, rotation + 1.
//   • Save: "Item", "ItemRotation", "ItemDropChance", "Facing" (3D data
//     value), "Invisible", "Fixed", "block_pos".
//
// WIRE
//   rotation (bits 0-2) and invisible (bit 3) ride the variant byte; the
//   framed item rides ItemFrameDataS2C (sent on first sight and whenever it
//   changes — MC's DATA_ITEM). Facing: HangingEntity.
//
// NOT HERE: framed maps (no map system), so a frame is always 12×12.
#pragma once

#include "common/entity/decoration/HangingEntity.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/block/BlockInteraction.hpp"

namespace Game {

    class ItemFrame : public HangingEntity {
    public:
        // `glow` picks the glow item frame (MC GlowItemFrame).
        ItemFrame(EntityLevel* level, bool glow);

        static constexpr int    kNumRotations = 8;
        static constexpr double kDepth = 0.0625;
        static constexpr double kWidth = 0.75;
        static constexpr uint8_t kVariantInvisible = 0x08;

        bool IsGlow() const { return m_glow; }

        // ── The framed item (MC DATA_ITEM / DATA_ROTATION) ─────────────────
        const ItemStack& GetItem() const { return m_item; }
        // MC setItem(stack, updateNeighbours): one of it, the add sound when
        // it is not empty, and the comparator behind told.
        void SetItem(const ItemStack& stack, bool updateNeighbours = true);
        int  GetRotation() const { return m_rotation; }
        void SetRotation(int rotation, bool updateNeighbours = true);
        // MC getAnalogOutput.
        int  GetAnalogOutput() const { return m_item.IsEmpty() ? 0 : m_rotation % kNumRotations + 1; }

        bool IsFixed() const { return m_fixed; }
        void SetFixed(bool fixed) { m_fixed = fixed; }
        bool IsInvisible() const { return m_invisible; }
        void SetInvisible(bool invisible) { m_invisible = invisible; }
        float GetDropChance() const { return m_dropChance; }
        void  SetDropChance(float chance) { m_dropChance = chance; }

        // The tracker's send-on-change latch for ItemFrameDataS2C.
        // Read-and-clear.
        bool ConsumeItemDirty() {
            const bool was = m_itemDirty;
            m_itemDirty = false;
            return was;
        }
        // The framed item with no sound and no neighbour update: the
        // client's copy from ItemFrameDataS2C, and a frame read from disk
        // (the tracker sends a non-empty item on first sight regardless).
        void SetItemSilently(const ItemStack& stack) {
            m_item = stack;
            if (!m_item.IsEmpty()) m_item.count = 1;   // setItem's copyWithCount(1)
        }

        // MC ItemFrame.interact — server side. `held` is the player's hand
        // stack; placing an item consumes one of it (outside creative).
        UseResult Interact(LivingEntity& player, ItemStack& held);

        // MC getPickResult: the framed item, else the frame.
        ItemStack PickResult() const;

        // MC ItemFrame.setDirection: any of the six faces.
        void SetDirection(Direction direction) override;
        // MC ItemFrame.survives.
        bool Survives() const override;

        // MC ItemFrame.dropItem(level, causedBy): break sound, frame + item.
        void DropItem(Entity* causedBy) override;
        void PlayPlacementSound() override;

        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        float BaseBbWidth() const override { return static_cast<float>(kWidth); }
        float BaseBbHeight() const override { return static_cast<float>(kWidth); }

        uint8_t GetVariantByte() const override {
            return static_cast<uint8_t>((m_rotation & 7) | (m_invisible ? kVariantInvisible : 0));
        }
        void SetVariantByte(uint8_t v) override;

    protected:
        AABBd CalculateBoundingBox(const glm::ivec3& pos, Direction direction) const override;

    private:
        // MC dropItem(level, causedBy, withFrame).
        void DropItem(Entity* causedBy, bool withFrame);
        ItemStack FrameItemStackWithData() const;
        void UpdateComparatorBehind();

        // The sounds, frame or glow frame.
        const char* AddItemSound() const;
        const char* RemoveItemSound() const;
        const char* RotateItemSound() const;
        const char* BreakSound() const;
        const char* PlaceSound() const;

        bool      m_glow = false;
        ItemStack m_item{};
        int       m_rotation = 0;
        float     m_dropChance = 1.0f;   // MC DEFAULT_DROP_CHANCE
        bool      m_fixed = false;
        bool      m_invisible = false;
        bool      m_itemDirty = false;
    };

} // namespace Game
