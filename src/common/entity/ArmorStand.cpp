// File: src/common/entity/ArmorStand.cpp
#include "common/entity/ArmorStand.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/data/DataComponents.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    ArmorStand::ArmorStand(EntityLevel* level)
        : Mob(EntityTypeId::ArmorStand, level, NoAiTag{}) {
        // MC ArmorStand.createAttributes: createLivingAttributes + STEP_HEIGHT 0.
        CreateLivingAttributes(m_attributes);
        if (m_attributes.Has(Attribute::StepHeight)) m_attributes.SetBaseValue(Attribute::StepHeight, 0.0);
        else                                         m_attributes.Register(Attribute::StepHeight, 0.0);
        m_health = GetMaxHealth();
        ClearHoldsEntityRefs();
    }

    // ── Dimensions ─────────────────────────────────────────────────────────

    float ArmorStand::BaseBbWidth() const {
        // MC getDefaultDimensions → getDimensionsMarker: MARKER_DIMENSIONS
        // (0×0), BABY_DIMENSIONS (the type's box scaled by 0.5) or the type's.
        if (IsMarker()) return 0.0f;
        return IsSmall() ? TypeInfo().width * 0.5f : TypeInfo().width;
    }

    float ArmorStand::BaseBbHeight() const {
        if (IsMarker()) return 0.0f;
        return IsSmall() ? TypeInfo().height * 0.5f : TypeInfo().height;
    }

    float ArmorStand::BaseEyeHeight() const {
        // BABY_DIMENSIONS.withEyeHeight(0.9875F); a marker keeps the type's
        // eye (MC's fixed(0,0) has eye 0, but nothing reads it).
        return IsSmall() ? 0.9875f : TypeInfo().eyeHeight;
    }

    // ── Poses / equipment ──────────────────────────────────────────────────

    void ArmorStand::SetPose(const Pose& pose) {
        m_pose = pose;
        MarkDataDirty();
    }

    int ArmorStand::SlotIndex(EquipmentSlot slot) {
        switch (slot) {
            case EquipmentSlot::MAINHAND: return 0;
            case EquipmentSlot::OFFHAND:  return 1;
            case EquipmentSlot::FEET:     return 2;
            case EquipmentSlot::LEGS:     return 3;
            case EquipmentSlot::CHEST:    return 4;
            case EquipmentSlot::HEAD:     return 5;
            default:                      return -1;
        }
    }

    int ArmorStand::FilterBit(EquipmentSlot slot, int offset) {
        // MC EquipmentSlot's filterBit column: MAINHAND 0, OFFHAND 5, FEET 1,
        // LEGS 2, CHEST 3, HEAD 4, BODY 6, SADDLE 7.
        int bit = 0;
        switch (slot) {
            case EquipmentSlot::MAINHAND: bit = 0; break;
            case EquipmentSlot::OFFHAND:  bit = 5; break;
            case EquipmentSlot::FEET:     bit = 1; break;
            case EquipmentSlot::LEGS:     bit = 2; break;
            case EquipmentSlot::CHEST:    bit = 3; break;
            case EquipmentSlot::HEAD:     bit = 4; break;
            case EquipmentSlot::BODY:     bit = 6; break;
            case EquipmentSlot::SADDLE:   bit = 7; break;
        }
        return bit + offset;
    }

    const ItemStack& ArmorStand::GetItemBySlot(EquipmentSlot slot) const {
        static const ItemStack kEmpty;
        const int i = SlotIndex(slot);
        return i < 0 ? kEmpty : m_equipment[static_cast<size_t>(i)];
    }

    ItemStack* ArmorStand::EquipmentInSlot(EquipmentSlot slot) {
        const int i = SlotIndex(slot);
        return i < 0 ? nullptr : &m_equipment[static_cast<size_t>(i)];
    }

    void ArmorStand::SetItemSlot(EquipmentSlot slot, const ItemStack& stack) {
        const int i = SlotIndex(slot);
        if (i < 0) return;
        m_equipment[static_cast<size_t>(i)] = stack;
        MarkDataDirty();
    }

    bool ArmorStand::CanUseSlot(EquipmentSlot slot) const {
        return slot != EquipmentSlot::BODY && slot != EquipmentSlot::SADDLE && !IsDisabled(slot);
    }

    EquipmentSlot ArmorStand::GetEquipmentSlotForItem(const ItemStack& stack) const {
        if (const auto equippable = stack.get(DataComponents::EQUIPPABLE)) {
            if (CanUseSlot(equippable->slot)) return equippable->slot;
        }
        return EquipmentSlot::MAINHAND;
    }

    bool ArmorStand::IsDisabled(EquipmentSlot slot) const {
        return (m_disabledSlots & (1 << FilterBit(slot, 0))) != 0 ||
               (IsHandSlot(slot) && !ShowArms());
    }

    // ── Interaction ────────────────────────────────────────────────────────

    EquipmentSlot ArmorStand::GetClickedSlot(const glm::vec3& location) const {
        // MC ArmorStand.getClickedSlot, verbatim: the click height in the
        // stand's own scale picks feet / chest / legs / head, each only when
        // that slot holds something; else the off hand when the main hand is
        // empty and the off hand is not; else the main hand.
        EquipmentSlot slotClicked = EquipmentSlot::MAINHAND;
        const bool small = IsSmall();
        const double ageScale = IsBaby() ? 0.5 : 1.0;
        const double y = static_cast<double>(location.y) / (static_cast<double>(scale) * ageScale);
        if (y >= 0.1 && y < 0.1 + (small ? 0.8 : 0.45) && HasItemInSlot(EquipmentSlot::FEET)) {
            slotClicked = EquipmentSlot::FEET;
        } else if (y >= 0.9 + (small ? 0.3 : 0.0) && y < 0.9 + (small ? 1.0 : 0.7) &&
                   HasItemInSlot(EquipmentSlot::CHEST)) {
            slotClicked = EquipmentSlot::CHEST;
        } else if (y >= 0.4 && y < 0.4 + (small ? 1.0 : 0.8) && HasItemInSlot(EquipmentSlot::LEGS)) {
            slotClicked = EquipmentSlot::LEGS;
        } else if (y >= 1.6 && HasItemInSlot(EquipmentSlot::HEAD)) {
            slotClicked = EquipmentSlot::HEAD;
        } else if (!HasItemInSlot(EquipmentSlot::MAINHAND) && HasItemInSlot(EquipmentSlot::OFFHAND)) {
            slotClicked = EquipmentSlot::OFFHAND;
        }
        return slotClicked;
    }

    bool ArmorStand::SwapItem(LivingEntity& player, EquipmentSlot slot, ItemStack& playerStack) {
        // MC ArmorStand.swapItem, verbatim.
        const ItemStack itemStack = GetItemBySlot(slot);
        if (!itemStack.IsEmpty() && (m_disabledSlots & (1 << FilterBit(slot, kDisableTakingOffset))) != 0) {
            return false;
        }
        if (itemStack.IsEmpty() && (m_disabledSlots & (1 << FilterBit(slot, kDisablePuttingOffset))) != 0) {
            return false;
        }
        if (player.IsCreative() && itemStack.IsEmpty() && !playerStack.IsEmpty()) {
            // hasInfiniteMaterials: a copy of one onto the stand, the hand
            // untouched.
            ItemStack one = playerStack;
            one.count = 1;
            SetItemSlot(slot, one);
            return true;
        }
        if (!playerStack.IsEmpty() && playerStack.count > 1) {
            if (!itemStack.IsEmpty()) return false;
            // split(1): one onto the stand, the rest stays in the hand.
            ItemStack one = playerStack;
            one.count = 1;
            SetItemSlot(slot, one);
            playerStack.count -= 1;
            return true;
        }
        SetItemSlot(slot, playerStack);
        playerStack = itemStack;
        return true;
    }

    UseResult ArmorStand::Interact(LivingEntity& player, ItemStack& held, const glm::vec3& location) {
        // MC ArmorStand.interact(player, hand, location). Entity.interact
        // (the super) is the leash branch, which a stand cannot take: PASS.
        if (IsMarker() || held.itemId == Items::NameTag) return UseResult::Pass;
        if (player.IsSpectator()) return UseResult::Success;
        if (m_level && m_level->IsClientSide()) return UseResult::SuccessServer;

        const EquipmentSlot itemInHandSlot = GetEquipmentSlotForItem(held);
        if (held.IsEmpty()) {
            const EquipmentSlot clickedSlot = GetClickedSlot(location);
            const EquipmentSlot targetSlot  = IsDisabled(clickedSlot) ? itemInHandSlot : clickedSlot;
            if (HasItemInSlot(targetSlot) && SwapItem(player, targetSlot, held)) {
                return UseResult::SuccessServer;
            }
        } else {
            if (IsDisabled(itemInHandSlot)) return UseResult::Fail;
            if (IsHandSlot(itemInHandSlot) && !ShowArms()) return UseResult::Fail;
            if (SwapItem(player, itemInHandSlot, held)) return UseResult::SuccessServer;
        }
        return UseResult::Pass;
    }

    // ── Damage ─────────────────────────────────────────────────────────────

    ItemStack ArmorStand::PickResult() {
        return ItemStack(Items::ArmorStand, 1);
    }

    void ArmorStand::Kill(Entity* attributedTo) {
        // MC kill(level, attributedTo): remove as KILLED; the ENTITY_DIE game
        // event has no listener here.
        (void)attributedTo;
        Remove(RemovalReason::Killed);
    }

    void ArmorStand::PlayBrokenSound() const {
        // MC ArmorStand.playBrokenSound: level.playSound(null, ...,
        // ARMOR_STAND_BREAK, getSoundSource(), 1, 1).
        if (m_level) m_level->PlaySound(nullptr, position, SoundEvents::ARMOR_STAND_BREAK, GetSoundSource(), 1.0f, 1.0f);
    }

    void ArmorStand::ShowBreakingParticles() const {
        // MC: ten BLOCK particles of oak planks around the body. No block
        // particle kind exists client-side (see the header); the sound and
        // the drop are the visible half.
    }

    void ArmorStand::CauseDamage(MobDamageSource source, Entity* attacker, float dmg) {
        float health = GetHealth();
        health -= dmg;
        if (health <= 0.5f) {
            BrokenByAnything(attacker);
            Kill(attacker);
        } else {
            SetHealth(health);
            (void)source;
        }
    }

    void ArmorStand::BrokenByPlayer(Entity* attacker) {
        // MC: the stand item, carrying the stand's custom name as its
        // CUSTOM_NAME, popped at the feet, then everything it wore.
        ItemStack result = PickResult();
        if (const auto& name = GetCustomName()) result.components.set(DataComponents::CUSTOM_NAME, *name);
        if (m_level) m_level->SpawnItemStackDrop(glm::dvec3(BlockPosition()) + glm::dvec3(0.5, 0.0, 0.5),
                                                 result);
        BrokenByAnything(attacker);
    }

    void ArmorStand::BrokenByAnything(Entity* attacker) {
        (void)attacker;
        PlayBrokenSound();
        // dropAllDeathLoot: an armor stand has no loot table. Every slot is
        // emptied and popped one block up (blockPosition().above()).
        const glm::dvec3 above = glm::dvec3(BlockPosition()) + glm::dvec3(0.5, 1.0, 0.5);
        for (ItemStack& stack : m_equipment) {
            if (stack.IsEmpty()) continue;
            if (m_level) m_level->SpawnItemStackDrop(above, stack);
            stack.Clear();
        }
        MarkDataDirty();
    }

    bool ArmorStand::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC ArmorStand.hurtServer, on the engine's damage sources. The tags
        // MC tests map as follows:
        //   BYPASSES_INVULNERABILITY   Void
        //   IS_EXPLOSION               Explosion
        //   IGNITES_ARMOR_STANDS       Lava, and Fire while not yet burning
        //                              (in_fire: the block under it)
        //   BURNS_ARMOR_STANDS         Fire while burning (on_fire: the tick)
        //   CAN_BREAK_ARMOR_STAND      PlayerAttack, MobAttack, Projectile
        //   ALWAYS_KILLS_ARMOR_STANDS  Projectile (arrow, trident)
        (void)amount;
        if (!m_level || m_level->IsClientSide()) return false;
        if (IsRemoved()) return false;
        if (!m_level->MobGriefing() && attacker && !attacker->IsPlayer() &&
            dynamic_cast<Mob*>(attacker) != nullptr) {
            return false;
        }
        if (source == MobDamageSource::Void) {
            Kill(attacker);
            return false;
        }
        if (IsInvulnerable() || IsInvisible() || IsMarker()) return false;

        if (source == MobDamageSource::Explosion) {
            BrokenByAnything(attacker);
            Kill(attacker);
            return false;
        }
        if (source == MobDamageSource::Lava || (source == MobDamageSource::Fire && !IsOnFire())) {
            if (IsOnFire()) CauseDamage(source, attacker, 0.15f);
            else            IgniteForSeconds(5);
            return false;
        }
        if (source == MobDamageSource::Fire) {
            if (GetHealth() > 0.5f) CauseDamage(source, attacker, 4.0f);
            return false;
        }

        const bool allowIncrementalBreaking =
            source == MobDamageSource::PlayerAttack || source == MobDamageSource::MobAttack ||
            source == MobDamageSource::Projectile;
        const bool shouldKill = source == MobDamageSource::Projectile;
        if (!allowIncrementalBreaking && !shouldKill) return false;

        // MC: a player who may not build (adventure) does nothing — the
        // engine has no mayBuild; every player builds.
        if (attacker && attacker->IsPlayer() && attacker->IsCreative()) {
            // source.isCreativePlayer(): broken outright, nothing dropped.
            PlayBrokenSound();
            ShowBreakingParticles();
            Kill(attacker);
            return true;
        }

        const int64_t time = m_level->GetGameTime();
        if (time - m_lastHit > static_cast<int64_t>(kWobbleTime) && !shouldKill) {
            m_level->BroadcastEntityEvent(*this, kEventHit);
            m_lastHit = time;
        } else {
            BrokenByPlayer(attacker);
            ShowBreakingParticles();
            Kill(attacker);
        }
        return true;
    }

    void ArmorStand::HandleEntityEvent(uint8_t id) {
        if (id == kEventHit) {
            // MC: the hit sound (0.3) and the wobble clock, client side.
            if (m_level && m_level->IsClientSide()) {
                // MC: level.playLocalSound(..., ARMOR_STAND_HIT, source, 0.3, 1).
                m_level->PlayLocalSound(position, SoundEvents::ARMOR_STAND_HIT, GetSoundSource(), 0.3f, 1.0f, false);
                m_lastHit = m_level->GetGameTime();
            }
            return;
        }
        Mob::HandleEntityEvent(id);
    }

    // ── Ticking ────────────────────────────────────────────────────────────

    bool ArmorStand::IsEffectiveAi() const {
        return Mob::IsEffectiveAi() && HasPhysics();
    }

    void ArmorStand::Travel(const glm::dvec3& input) {
        // MC ArmorStand.travel: gravity and the ground only while hasPhysics
        // — a marker or a weightless stand never moves on its own.
        if (HasPhysics()) LivingEntity::Travel(input);
    }

    void ArmorStand::Tick() {
        Mob::Tick();
        // MC tickHeadTurn / setYBodyRot / setYHeadRot: a stand's body and head
        // always face where the stand faces. LivingEntity's tick turned the
        // body toward the travel direction; put it back.
        yBodyRot = yRot;
        yHeadRot = yRot;
        yBodyRotO = yRotO;
        yHeadRotO = yRotO;
    }

} // namespace Game
