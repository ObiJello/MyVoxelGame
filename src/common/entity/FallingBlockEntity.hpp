// File: src/common/entity/FallingBlockEntity.hpp
//
// MC net.minecraft.world.entity.item.FallingBlockEntity — a block in mid-air.
//
// ARCHITECTURE, and it is a deliberate deviation: MC models this as a plain
// Entity. This engine derives Game::Mob instead, with every piece of mob
// machinery inert — no goals, Tick overridden wholesale, Hurt rejected,
// MobCategory::Misc so it never touches a spawn cap or census. That is the
// same trade Projectile.hpp documents and takes for the same reason: the
// tracking, wire, NBT and client-factory pipelines are all built around
// Game::Mob, and a third entity kind would mean a third manager, a third
// packet family and a third renderer path for two classes.
//
// TICK ORDER IS THE FEATURE. MC's sequence is
//
//     discard if the carried state is air
//     ++time
//     applyGravity              (-0.04 on y)
//     move(SELF, velocity)
//     [landing branch]          -- which itself does velocity *= (0.7,-0.5,0.7)
//     velocity *= 0.98          -- ALWAYS last, outside every branch
//
// The 0.98 sitting outside the branch is not a detail: applying it before the
// landing damp, or skipping it on the tick the entity lands, changes how far a
// block slides on impact and how a diagonal fall drifts. See PrimedTnt, which
// runs the SAME two operations in the opposite order — that difference is real
// and is why the two classes do not share a movement helper.
#pragma once

#include "common/entity/Mob.hpp"
#include "common/world/block/BlockState.hpp"

namespace Game {

    class FallingBlockEntity : public Mob {
    public:
        explicit FallingBlockEntity(EntityLevel* level);

        // ── MC constants (FallingBlockEntity.java) ─────────────────────────
        static constexpr double kGravity            = 0.04;
        static constexpr double kAirDrag            = 0.98;
        // On landing: horizontal keeps 0.7, vertical flips and halves.
        static constexpr double kLandHorizontal     = 0.7;
        static constexpr double kLandVertical       = -0.5;
        // Despawn: past 100 ticks AND outside build height, or 600 regardless.
        static constexpr int    kOutOfWorldGrace    = 100;
        static constexpr int    kMaxLifetime        = 600;
        static constexpr int    kDefaultFallHurtMax = 40;

        // MC FallingBlockEntity.fall(level, pos, state) — position at the
        // cell's horizontal centre with zero velocity, and remember where it
        // started (the renderer seeds its model randomisation from that, so a
        // falling gravel block keeps the texture rotation it had as a block).
        //
        // Does NOT clear the source cell; the caller has already done that,
        // because only it knows whether the cell should become air or water.
        void InitFall(const glm::ivec3& blockPos, BlockState state);

        void Tick() override;

        // ── The tick, in two halves ────────────────────────────────────────
        //
        // Tick() is exactly TickPhysics() followed by TickLanding(). The split
        // exists so the server can run the first half for a hundred thousand
        // falling blocks across the worker pool: it reads the world and
        // writes only this entity, so it is safe alongside any other entity's
        // first half. The second half may WRITE the world (it becomes a block,
        // or pops as an item) and stays serial, in insertion order — see
        // MobManager::Tick, which also owns the rule that makes the split
        // exact: a block placed by an earlier entity this tick invalidates the
        // physics of any later entity whose swept region it touched, and that
        // entity re-runs its first half serially against the updated world.
        //
        // What TickPhysics left to do:
        //   Discarded    — the entity removed itself (an air state); nothing
        //                  else runs.
        //   Airborne     — still falling. The rest of the tick (the expiry
        //                  test and the drag) was per-entity and has ALREADY
        //                  run; the landing half must not.
        //   NeedsLanding — on the ground, concrete powder, or expiring: the
        //                  landing half runs, serially.
        // The Airborne fast path is what keeps the serial half from walking
        // every entity on every tick of the fall — for a hundred thousand
        // falling blocks that walk was 8.6 ms of cache misses a tick that
        // ended in "nothing to do" for all but the last tick.
        enum class PhysicsOutcome : uint8_t { Discarded, Airborne, NeedsLanding };
        PhysicsOutcome TickPhysics();
        // `outLandingCell` (optional) receives the cell TryLand was asked to
        // fill when the block touched down this tick; returns whether it did.
        // That cell is the ONLY block position the landing half writes.
        bool TickLanding(glm::ivec3* outLandingCell);

        // Everything TickPhysics mutates, so a parallel result can be undone
        // and recomputed serially. oldPosition/oldRot are NOT here — they are
        // set by the pre-tick, before the physics runs, and are left alone.
        // `half` and `gravity` are copied in so the conflict test that reads
        // the snapshot never has to touch the entity (a cache miss each).
        struct PhysicsSnapshot {
            glm::dvec3 position{0.0};
            glm::dvec3 velocity{0.0};
            glm::vec3  half{0.0f};
            double     gravity = 0.0;
            float      fallDistance = 0.0f;
            bool       onGround = false;
            bool       horizontalCollision = false;
            bool       verticalCollision = false;
            int        time = 0;
        };
        PhysicsSnapshot CapturePhysics() const;
        void RestorePhysics(const PhysicsSnapshot& snap);

        // ── Mob machinery, switched off ────────────────────────────────────
        double GetGravity() const override { return kGravity; }
        // MC hurtServer: marks hurt and returns false — a falling anvil cannot
        // be shot out of the air.
        bool Hurt(MobDamageSource, float, Entity*) override { return false; }
        bool IsPushable() const override { return false; }
        // Same deliberate divergence as PrimedTnt: vanilla's
        // FallingBlockEntity.isPickable is !isRemoved(), so a column of falling
        // sand blocks every click that passes through it. Here it does not.
        bool IsPickable() const override { return false; }
        void CheckDespawn() override {}                       // has its own timer
        bool RemoveWhenFarAway(double) const override { return false; }
        // MC displayFireAnimation() == false: a falling block dropped through
        // lava does not render on fire.
        bool FireImmune() const override { return true; }

        // ── State (all of it round-trips through NBT) ──────────────────────
        BlockState CarriedState() const { return m_blockState; }
        void       SetCarriedState(BlockState s) { m_blockState = s; }

        glm::ivec3 StartPos() const { return m_startPos; }
        void       SetStartPos(const glm::ivec3& p) { m_startPos = p; }

        int  Time() const { return m_time; }
        void SetTime(int t) { m_time = t; }

        bool DropsItem() const { return m_dropItem; }
        void SetDropsItem(bool v) { m_dropItem = v; }

        // MC disableDrop() — what BrushableBlock calls so a suspicious sand
        // block that shatters on landing yields nothing.
        bool CancelDrop() const { return m_cancelDrop; }
        void SetCancelDrop(bool v) { m_cancelDrop = v; }

        bool  HurtsEntities() const { return m_hurtEntities; }
        float FallDamagePerDistance() const { return m_fallDamagePerDistance; }
        int   FallDamageMax() const { return m_fallDamageMax; }

        // MC setHurtsEntities(perDistance, max) — AnvilBlock passes (2.0, 40),
        // a dripstone tip passes (1.0 * size, 40).
        void SetHurtsEntities(float perDistance, int maxDamage) {
            m_hurtEntities = true;
            m_fallDamagePerDistance = perDistance;
            m_fallDamageMax = maxDamage;
        }
        void SetHurtsEntitiesRaw(bool on, float perDistance, int maxDamage) {
            m_hurtEntities = on;
            m_fallDamagePerDistance = perDistance;
            m_fallDamageMax = maxDamage;
        }

        // MC causeFallDamage — the anvil bite. Damages every living entity
        // inside the falling block's OWN box (not a radius), then rolls the
        // anvil chip.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;

    private:
        // The landing branch of tick(): try to become a block again, or fail
        // and pop as an item.
        void TryLand(const glm::ivec3& pos);
        void DropAsItem();

        BlockState m_blockState{};
        glm::ivec3 m_startPos{0, 0, 0};
        int        m_time = 0;
        bool       m_dropItem = true;
        bool       m_cancelDrop = false;
        bool       m_hurtEntities = false;
        float      m_fallDamagePerDistance = 0.0f;
        int        m_fallDamageMax = kDefaultFallHurtMax;
    };

} // namespace Game
