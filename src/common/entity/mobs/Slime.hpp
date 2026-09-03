// File: src/common/entity/mobs/Slime.hpp
//
// MC net.minecraft.world.entity.monster.Slime.
//
// The slime is its SIZE: health is size², speed 0.2 + 0.1·size, damage size,
// hitbox 0.52·size — one synched integer drives everything, which is why
// setSize exists instead of a constructor parameter (splitting halves it in
// place). Movement is not a navigation at all: SlimeMoveControl turns toward
// a heading its goals choose and hops, with the jump cadence (10..30 ticks,
// ÷3 while chasing) doing the travelling.
//
// Contact damage replaces melee goals — MC deals it from playerTouch/push;
// here the server checks reach + line of sight each tick, which is the same
// observable rule (isDealsDamage: never for size 1).
//
// On a lethal removal a slime of size > 1 splits into 2-4 of half size at
// +0.5 blocks — the mechanism slime farms are built around.
#pragma once

#include "common/entity/Mob.hpp"
#include "common/entity/ai/Controls.hpp"

namespace Game {

    class Slime;

    // MC Slime.SlimeMoveControl — a heading + hop controller. Goals feed it a
    // direction (SetDirection) and arm one tick of movement (SetWantedMovement).
    class SlimeMoveControl : public MoveControl {
    public:
        explicit SlimeMoveControl(Slime* slime);

        void SetDirection(float yRot, bool aggressive) {
            m_yRot = yRot;
            m_aggressive = aggressive;
        }
        void SetWantedMovement(double speedModifier) {
            m_speedModifier = speedModifier;
            m_operation = Operation::MoveTo;
        }

        void Tick() override;

    private:
        Slime* m_slime;
        float  m_yRot = 0.0f;
        int    m_jumpDelay = 0;
        bool   m_aggressive = false;
    };

    class Slime : public Mob {
    public:
        explicit Slime(EntityTypeId type, EntityLevel* level);
        static std::unique_ptr<Slime> Make(EntityLevel* level) {
            return std::make_unique<Slime>(EntityTypeId::Slime, level);
        }

        int  GetSize() const { return m_size; }
        // MC setSize: clamp 1..127, rewrite the size-derived attribute bases.
        void SetSize(int size, bool resetHealth);

        // MC Slime.setSize (Slime.java:100): xpReward = the size — a big
        // slime pays 4, and each split child pays its own size again when it
        // dies credited. MagmaCube inherits, as in MC.
        int GetXpReward() const override { return m_size; }

        bool IsTiny() const { return m_size <= 1; }
        // MC isDealsDamage — a tiny slime is harmless (a magma cube never is).
        virtual bool DealsDamage() const { return !IsTiny() && IsEffectiveAi(); }

        // MC getJumpDelay: 10..29 ticks between hops.
        virtual int GetJumpDelay();

        // MC getAttackDamage — the magma cube adds a flat +2.
        virtual float GetAttackDamageValue() const {
            return static_cast<float>(GetAttributeValue(Attribute::AttackDamage));
        }

        // MC decreaseSquish's factor — 0.6 slime, 0.9 magma cube (the slower
        // decay is the lava-blob's oozier look).
        virtual float SquishDecay() const { return 0.6f; }

        // MC dimensions scale linearly with size.
        float BaseBbWidth()   const override { return 0.52f * m_size; }
        float BaseBbHeight()  const override { return 0.52f * m_size; }
        float BaseEyeHeight() const override { return 0.52f * m_size * 0.625f; }

        // MC Slime.jumpFromGround: the hop is EXACTLY jump power, not the
        // max-with-current-motion the base takes — a slime mid-bounce does
        // not stack hops.
        void JumpFromGround() override;

        void Tick() override;
        void TickDeath() override;

        // The size IS the slime's wire variant.
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_size); }
        void    SetVariantByte(uint8_t v) override { SetSize(v, false); }

        // MC Slime.finalizeSpawn: size = 1 << rand(3), the top step promoted
        // half the time scaled by difficulty.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // Squish state, mirrored for the renderer's stretch.
        float GetSquish(float partialTick) const {
            return m_oSquish + partialTick * (m_squish - m_oSquish);
        }

    protected:
        void RegisterGoals() override;

        // The split children must be the CONCRETE class — a dying magma cube
        // splits into magma cubes, not slimes (MC convertTo keeps the type).
        virtual std::unique_ptr<Slime> MakeSplitChild() {
            return std::make_unique<Slime>(GetType(), m_level);
        }

    private:
        void DealContactDamage();

        int   m_size = 1;
        float m_squish = 0.0f, m_oSquish = 0.0f, m_targetSquish = 0.0f;
        bool  m_wasOnGround = false;
    };

    // MC MagmaCube — a slime tuned for lava: fire-immune, immune to fall
    // damage, hops a quarter as often but higher (+0.1 per size), and its
    // touch hurts at EVERY size, two points harder.
    class MagmaCube : public Slime {
    public:
        explicit MagmaCube(EntityLevel* level)
            : Slime(EntityTypeId::MagmaCube, level) {}

        bool FireImmune() const override { return true; }
        bool DealsDamage() const override { return IsEffectiveAi(); }
        int  GetJumpDelay() override { return Slime::GetJumpDelay() * 4; }
        void JumpFromGround() override {
            velocity.y = GetJumpPower() + static_cast<float>(GetSize()) * 0.1f;
            needsSync = true;
        }
        bool CauseFallDamage(double, float) override { return false; }

        float GetAttackDamageValue() const override {
            return Slime::GetAttackDamageValue() + 2.0f;
        }
        float SquishDecay() const override { return 0.9f; }

    protected:
        std::unique_ptr<Slime> MakeSplitChild() override {
            return std::make_unique<MagmaCube>(m_level);
        }
    };

} // namespace Game
