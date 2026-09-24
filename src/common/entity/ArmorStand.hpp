// File: src/common/entity/ArmorStand.hpp
//
// MC net.minecraft.world.entity.decoration.ArmorStand — a LivingEntity in
// MC (it has health, equipment and a hurt animation) that is not a Mob (no
// AI, no navigation). Here it rides the Mob pipeline the way the primed TNT,
// the falling block and the end crystal do (Mob's NoAiTag): one manager,
// one tracker, one client store, and none of the goal machinery is ever
// built for it.
//
// WHAT IS HERE, IN MC'S TERMS
//   • The synched client flags (small, show arms, no base plate, marker) and
//     the six part poses, saved as MC saves them ("Pose", "ShowArms", ...).
//   • Equipment: the six humanoid slots, swapped by right-click (interact:
//     the clicked HEIGHT picks the slot the way MC's getClickedSlot does),
//     and disabled per slot by "DisabledSlots" (MC's three 8-bit groups:
//     remove / take / put).
//   • hurtServer's whole ladder: creative breaks it outright, a survival hit
//     wobbles it (entity event 32) and a second hit within five ticks
//     breaks it and drops the stand and everything it wears; explosions
//     break it, fire burns it, the void kills it, a marker or an invisible
//     stand takes nothing.
//   • Physics: gravity and the ground (LivingEntity.travel) unless it is a
//     marker or weightless; never pushed, never pushing.
//
// WHAT THE ENGINE HAS NO HOME FOR (and this port says so instead of faking)
//   • (Sounds are real: break / hit / fall — see PlayBrokenSound,
//     HandleEntityEvent and GetFallSounds.)
//   • The oak-planks breaking particles (ParticleTypes.BLOCK): no block
//     particle kind exists client-side.
//   • Adventure-mode `mayBuild`, lightning immunity (no lightning),
//     minecart pushing (no minecarts). (isAffectedByPotions IS ported —
//     see the override below.)
//   • Skulls on the head (no skull renderer); a block or item on the head
//     draws (CustomHeadLayer's non-skull branch).
#pragma once

#include "common/entity/Mob.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockInteraction.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Game {

    class ArmorStand final : public Mob {
    public:
        explicit ArmorStand(EntityLevel* level);

        // ── MC's constants ──────────────────────────────────────────────────
        static constexpr int kWobbleTime = 5;
        // Rotations in DEGREES (MC Rotations), the defaults of each part.
        static constexpr glm::vec3 kDefaultHeadPose     {  0.0f, 0.0f,   0.0f };
        static constexpr glm::vec3 kDefaultBodyPose     {  0.0f, 0.0f,   0.0f };
        static constexpr glm::vec3 kDefaultLeftArmPose  {-10.0f, 0.0f, -10.0f };
        static constexpr glm::vec3 kDefaultRightArmPose {-15.0f, 0.0f,  10.0f };
        static constexpr glm::vec3 kDefaultLeftLegPose  { -1.0f, 0.0f,  -1.0f };
        static constexpr glm::vec3 kDefaultRightLegPose {  1.0f, 0.0f,   1.0f };
        // MC DISABLE_TAKING_OFFSET / DISABLE_PUTTING_OFFSET: the three 8-bit
        // groups of DisabledSlots.
        static constexpr int kDisableTakingOffset  = 8;
        static constexpr int kDisablePuttingOffset = 16;
        // MC DATA_CLIENT_FLAGS bits, plus one of the engine's own: MC's
        // "invisible" is a flag every entity shares; here it rides this byte
        // (the tracker's variant byte) because nothing else needs it.
        static constexpr uint8_t kFlagSmall       = 1;
        static constexpr uint8_t kFlagShowArms    = 4;
        static constexpr uint8_t kFlagNoBasePlate = 8;
        static constexpr uint8_t kFlagMarker      = 16;
        static constexpr uint8_t kFlagInvisible   = 32;
        // Entity event: the hit wobble (MC handleEntityEvent 32).
        static constexpr uint8_t kEventHit = 32;

        // The six poses as one record, MC ArmorStand.ArmorStandPose.
        struct Pose {
            glm::vec3 head     = kDefaultHeadPose;
            glm::vec3 body     = kDefaultBodyPose;
            glm::vec3 leftArm  = kDefaultLeftArmPose;
            glm::vec3 rightArm = kDefaultRightArmPose;
            glm::vec3 leftLeg  = kDefaultLeftLegPose;
            glm::vec3 rightLeg = kDefaultRightLegPose;
        };

        // ── Flags ───────────────────────────────────────────────────────────
        bool IsSmall()       const { return (m_clientFlags & kFlagSmall) != 0; }
        bool ShowArms()      const { return (m_clientFlags & kFlagShowArms) != 0; }
        bool ShowBasePlate() const { return (m_clientFlags & kFlagNoBasePlate) == 0; }
        bool IsMarker()      const { return (m_clientFlags & kFlagMarker) != 0; }
        bool IsInvisible()   const { return (m_clientFlags & kFlagInvisible) != 0; }
        void SetSmall(bool v)       { SetFlag(kFlagSmall, v); }
        void SetShowArms(bool v)    { SetFlag(kFlagShowArms, v); }
        void SetNoBasePlate(bool v) { SetFlag(kFlagNoBasePlate, v); }
        void SetMarker(bool v)      { SetFlag(kFlagMarker, v); }
        void SetInvisible(bool v)   { SetFlag(kFlagInvisible, v); }
        uint8_t GetClientFlags() const { return m_clientFlags; }

        // The whole byte is the tracker's variant byte: it reaches every
        // watcher on first sight and on change, nothing extra to wire.
        uint8_t GetVariantByte() const override { return m_clientFlags; }
        void    SetVariantByte(uint8_t v) override { m_clientFlags = v; }

        // MC isBaby: a small stand IS a baby (half box, the small model).
        bool IsBaby() const override { return IsSmall(); }
        float BaseBbWidth()   const override;
        float BaseBbHeight()  const override;
        float BaseEyeHeight() const override;

        // ── Poses ───────────────────────────────────────────────────────────
        const Pose& GetPose() const { return m_pose; }
        void SetPose(const Pose& pose);
        void SetHeadPose(const glm::vec3& r)     { m_pose.head = r;     MarkDataDirty(); }
        void SetBodyPose(const glm::vec3& r)     { m_pose.body = r;     MarkDataDirty(); }
        void SetLeftArmPose(const glm::vec3& r)  { m_pose.leftArm = r;  MarkDataDirty(); }
        void SetRightArmPose(const glm::vec3& r) { m_pose.rightArm = r; MarkDataDirty(); }
        void SetLeftLegPose(const glm::vec3& r)  { m_pose.leftLeg = r;  MarkDataDirty(); }
        void SetRightLegPose(const glm::vec3& r) { m_pose.rightLeg = r; MarkDataDirty(); }

        // ── Equipment ───────────────────────────────────────────────────────
        // MC LivingEntity.getItemBySlot / setItemSlot, the six humanoid
        // slots (EquipmentSlot::MAINHAND..HEAD). BODY and SADDLE are never
        // usable on a stand (canUseSlot) and are not stored.
        static constexpr int kEquipmentSlots = 6;
        const ItemStack& GetItemBySlot(EquipmentSlot slot) const;
        void SetItemSlot(EquipmentSlot slot, const ItemStack& stack);
        bool HasItemInSlot(EquipmentSlot slot) const { return !GetItemBySlot(slot).IsEmpty(); }
        int  GetDisabledSlots() const { return m_disabledSlots; }
        void SetDisabledSlots(int v) { m_disabledSlots = v; }
        // MC ArmorStand.canUseSlot.
        bool CanUseSlot(EquipmentSlot slot) const;
        // MC LivingEntity.getEquipmentSlotForItem: the item's EQUIPPABLE
        // slot when the stand may use it, MAINHAND otherwise.
        EquipmentSlot GetEquipmentSlotForItem(const ItemStack& stack) const;

        // The pose and the equipment changed since the tracker last sent
        // them (ArmorStandDataS2C). Server side; the tracker takes it.
        bool ConsumeDataDirty() { const bool d = m_dataDirty; m_dataDirty = false; return d; }
        void MarkDataDirty() { m_dataDirty = true; }

        // ── Interaction ─────────────────────────────────────────────────────
        // MC ArmorStand.interact(player, hand, location): `location` is the
        // click relative to the stand's position (feet), which is what picks
        // the slot when the hand is empty. Server side only — the client's
        // prediction is the plain swing. `held` is the player's hand stack;
        // a swap writes into it (the caller resyncs the inventory).
        UseResult Interact(LivingEntity& player, ItemStack& held, const glm::vec3& location);

        // ── Damage ──────────────────────────────────────────────────────────
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC ArmorStand.getFallSounds: ARMOR_STAND_FALL for both.
        FallSounds GetFallSounds() const override {
            return {"entity.armor_stand.fall", "entity.armor_stand.fall"};
        }
        void HandleEntityEvent(uint8_t id) override;
        // Client: the game time of the last hit, for the wobble
        // (ArmorStandRenderer's `wiggle`). -1000 = never.
        int64_t LastHit() const { return m_lastHit; }

        // ── Entity hooks ────────────────────────────────────────────────────
        void Tick() override;
        void Travel(const glm::dvec3& input) override;
        bool IsPushable() const override { return false; }
        bool IsPickable() const override { return !IsMarker(); }
        // MC ignoreExplosion: an invisible stand is passed over; a visible
        // one is broken by the blast (through Hurt).
        bool IgnoreExplosion() const override { return IsInvisible(); }
        // MC ArmorStand.isAffectedByPotions — false: splash potions and
        // lingering clouds pass it by.
        bool IsAffectedByPotions() const override { return false; }
        bool ExplosionPushOnly() const override { return false; }
        void CheckDespawn() override {}
        bool RemoveWhenFarAway(double) const override { return false; }
        // MC ArmorStand.isEffectiveAi: super && hasPhysics.
        bool IsEffectiveAi() const override;
        // MC attackable(): false — no mob ever targets a stand. The engine's
        // IsAttackable gates whether a PLAYER may hit it, which MC allows.
        bool IsAttackable() const override { return true; }
        // MC getMainArm: RIGHT, always.

        // The item this stand drops (MC getPickResult / brokenByPlayer).
        static ItemStack PickResult();

    private:
        void SetFlag(uint8_t bit, bool on) {
            m_clientFlags = on ? (m_clientFlags | bit) : static_cast<uint8_t>(m_clientFlags & ~bit);
        }
        // MC hasPhysics: not a marker, and not weightless.
        bool HasPhysics() const { return !IsMarker() && !IsNoGravity(); }

        // MC ArmorStand.getClickedSlot / isDisabled / swapItem.
        EquipmentSlot GetClickedSlot(const glm::vec3& location) const;
        bool IsDisabled(EquipmentSlot slot) const;
        bool SwapItem(LivingEntity& player, EquipmentSlot slot, ItemStack& playerStack);

        // MC hurtServer's helpers.
        void CauseDamage(MobDamageSource source, Entity* attacker, float dmg);
        void BrokenByPlayer(Entity* attacker);
        void BrokenByAnything(Entity* attacker);
        void PlayBrokenSound() const;
        void ShowBreakingParticles() const;
        void Kill(Entity* attributedTo);

        static int SlotIndex(EquipmentSlot slot);
        // MC EquipmentSlot.getFilterBit(offset): the slot's bit in one of the
        // three DisabledSlots groups.
        static int FilterBit(EquipmentSlot slot, int offset);
        static bool IsHandSlot(EquipmentSlot slot) {
            return slot == EquipmentSlot::MAINHAND || slot == EquipmentSlot::OFFHAND;
        }

        uint8_t  m_clientFlags = 0;
        int      m_disabledSlots = 0;
        int64_t  m_lastHit = -1000;
        Pose     m_pose;
        std::array<ItemStack, kEquipmentSlots> m_equipment;
        bool     m_dataDirty = false;
    };

} // namespace Game
