// File: src/common/entity/decoration/ItemFrame.cpp
#include "common/entity/decoration/ItemFrame.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/LegacySolid.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"

namespace Game {

    ItemFrame::ItemFrame(EntityLevel* level, bool glow)
        : HangingEntity(glow ? EntityTypeId::GlowItemFrame : EntityTypeId::ItemFrame, level),
          m_glow(glow) {
        RecalculateBoundingBox();
    }

    // ── Sounds (MC ItemFrame / GlowItemFrame get*Sound) ───────────────────

    const char* ItemFrame::AddItemSound() const {
        return m_glow ? SoundEvents::GLOW_ITEM_FRAME_ADD_ITEM : SoundEvents::ITEM_FRAME_ADD_ITEM;
    }
    const char* ItemFrame::RemoveItemSound() const {
        return m_glow ? SoundEvents::GLOW_ITEM_FRAME_REMOVE_ITEM : SoundEvents::ITEM_FRAME_REMOVE_ITEM;
    }
    const char* ItemFrame::RotateItemSound() const {
        return m_glow ? SoundEvents::GLOW_ITEM_FRAME_ROTATE_ITEM : SoundEvents::ITEM_FRAME_ROTATE_ITEM;
    }
    const char* ItemFrame::BreakSound() const {
        return m_glow ? SoundEvents::GLOW_ITEM_FRAME_BREAK : SoundEvents::ITEM_FRAME_BREAK;
    }
    const char* ItemFrame::PlaceSound() const {
        return m_glow ? SoundEvents::GLOW_ITEM_FRAME_PLACE : SoundEvents::ITEM_FRAME_PLACE;
    }

    // ── Geometry ───────────────────────────────────────────────────────────

    void ItemFrame::SetDirection(Direction direction) {
        // MC ItemFrame.setDirection: a wall frame turns by yRot; a floor /
        // ceiling frame tips by xRot = -90 × the direction's step (up: -90).
        m_direction = direction;
        if (IsHorizontal(direction)) {
            xRot = 0.0f;
            yRot = ToYRot(direction);
        } else {
            xRot = -90.0f * static_cast<float>(StepY(direction));
            yRot = 0.0f;
        }
        xRotO = xRot;
        yRotO = yRot;
        yBodyRot = yBodyRotO = yRot;
        yHeadRot = yHeadRotO = yRot;
        RecalculateBoundingBox();
    }

    AABBd ItemFrame::CalculateBoundingBox(const glm::ivec3& pos, Direction direction) const {
        // MC ItemFrame.createBoundingBox (no framed map: always 12×12 px).
        const glm::dvec3 center = glm::dvec3(pos) + glm::dvec3(0.5) - Step(direction) * kShiftToBlockWall;
        const Axis axis = AxisOf(direction);
        const glm::dvec3 size(axis == Axis::X ? kDepth : kWidth,
                              axis == Axis::Y ? kDepth : kWidth,
                              axis == Axis::Z ? kDepth : kWidth);
        return AABBd::FromMinMax(center - size * 0.5, center + size * 0.5);
    }

    bool ItemFrame::Survives() const {
        // MC ItemFrame.survives: a fixed frame always does; otherwise its box
        // clear of blocks, and the ONE block behind it solid (or, for a wall
        // frame, a repeater / comparator), and nothing else hanging there
        // facing the same way — another frame may share the space.
        if (m_fixed) return true;
        if (!m_level || !m_level->Blocks()) return false;
        if (HasLevelCollision(PopBox())) return false;
        const glm::ivec3 behind = m_pos - glm::ivec3(StepX(m_direction), StepY(m_direction), StepZ(m_direction));
        const BlockState state = m_level->Blocks()->GetBlockState(behind.x, behind.y, behind.z);
        const bool supported = IsLegacySolid(state) ||
                               (IsHorizontal(m_direction) && IsDiodeBlock(state.Block()));
        return supported && CanCoexist(true);
    }

    void ItemFrame::SetVariantByte(uint8_t v) {
        m_rotation  = v & 7;
        m_invisible = (v & kVariantInvisible) != 0;
        SyncFromNetwork();
    }

    // ── The framed item ────────────────────────────────────────────────────

    void ItemFrame::UpdateComparatorBehind() {
        // MC level.updateNeighbourForOutputSignal(pos, Blocks.AIR): a
        // comparator reading this frame through the wall re-reads it.
        if (!m_level || m_level->IsClientSide()) return;
        if (ILevelWrite* blocks = m_level->MutableBlocks()) {
            blocks->UpdateNeighbourForOutputSignal(m_pos, BlockID::Air);
        }
    }

    void ItemFrame::SetItem(const ItemStack& stack, bool updateNeighbours) {
        ItemStack one = stack;
        if (!one.IsEmpty()) one.count = 1;   // copyWithCount(1)
        m_item = one;
        m_itemDirty = true;
        if (!m_item.IsEmpty()) PlaySound(AddItemSound(), 1.0f, 1.0f);
        if (updateNeighbours) UpdateComparatorBehind();
    }

    void ItemFrame::SetRotation(int rotation, bool updateNeighbours) {
        m_rotation = ((rotation % kNumRotations) + kNumRotations) % kNumRotations;
        if (updateNeighbours) UpdateComparatorBehind();
    }

    UseResult ItemFrame::Interact(LivingEntity& player, ItemStack& held) {
        // MC ItemFrame.interact, the server half.
        (void)player;
        if (m_fixed) return UseResult::Pass;
        if (m_item.IsEmpty()) {
            if (held.IsEmpty() || IsRemoved()) return UseResult::Pass;
            SetItem(held);
            // itemStack.consume(1, player): creative keeps its stack through
            // the interact dispatch's count snapshot.
            held.count -= 1;
            if (held.count <= 0) held.Clear();
            return UseResult::Success;
        }
        PlaySound(RotateItemSound(), 1.0f, 1.0f);
        SetRotation(m_rotation + 1);
        return UseResult::Success;
    }

    ItemStack ItemFrame::FrameItemStackWithData() const {
        ItemStack stack(m_glow ? Items::GlowItemFrame : Items::ItemFrame, 1);
        if (const auto& name = GetCustomName()) stack.components.set(DataComponents::CUSTOM_NAME, *name);
        return stack;
    }

    ItemStack ItemFrame::PickResult() const {
        if (!m_item.IsEmpty()) {
            ItemStack copy = m_item;
            copy.count = 1;
            return copy;
        }
        return FrameItemStackWithData();
    }

    // ── Breaking ───────────────────────────────────────────────────────────

    bool ItemFrame::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC ItemFrame.hurtServer.
        if (!m_level || m_level->IsClientSide()) return false;
        // canHurtWhenFixed / isInvulnerableToBase: the void, or a creative
        // player.
        const bool creativePlayer = attacker && attacker->IsPlayer() && attacker->IsCreative();
        if (m_fixed) {
            return (source == MobDamageSource::Void || creativePlayer) &&
                   HangingEntity::Hurt(source, amount, attacker);
        }
        if (IsInvulnerable() && source != MobDamageSource::Void && !creativePlayer) return false;
        // shouldDamageDropItem: anything but an explosion knocks a framed
        // item out first; the frame breaks on the next hit.
        if (source != MobDamageSource::Explosion && !m_item.IsEmpty()) {
            DropItem(attacker, /*withFrame=*/false);
            PlaySound(RemoveItemSound(), 1.0f, 1.0f);
            return true;
        }
        return HangingEntity::Hurt(source, amount, attacker);
    }

    void ItemFrame::DropItem(Entity* causedBy) {
        PlaySound(BreakSound(), 1.0f, 1.0f);
        DropItem(causedBy, /*withFrame=*/true);
    }

    void ItemFrame::DropItem(Entity* causedBy, bool withFrame) {
        // MC ItemFrame.dropItem(level, causedBy, withFrame).
        if (m_fixed || !m_level) return;
        const ItemStack framed = m_item;
        SetItem(ItemStack{});
        if (!m_level->DoEntityDrops()) return;
        if (causedBy && causedBy->IsPlayer() && causedBy->IsCreative()) return;
        if (withFrame) SpawnAtLocation(FrameItemStackWithData());
        if (!framed.IsEmpty() && m_level->Random().NextFloat() < m_dropChance) {
            SpawnAtLocation(framed);
        }
    }

    void ItemFrame::PlayPlacementSound() {
        PlaySound(PlaceSound(), 1.0f, 1.0f);
    }

} // namespace Game
