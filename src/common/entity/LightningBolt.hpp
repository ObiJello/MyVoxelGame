// File: src/common/entity/LightningBolt.hpp
//
// MC net.minecraft.world.entity.LightningBolt — a lightning strike. A plain
// Entity in MC; here it rides the projectile Misc pipeline the way the other
// non-mob entities do (EvokerFangs, EyeOfEnder — see Projectile.hpp's
// architecture note): server tracking, AddEntityS2C and the client mob
// manager, with every bit of mob machinery inert (Tick is overridden
// wholesale, Hurt is refused).
//
// Timeline, transcribed from LightningBolt.tick():
//   life starts at 2 (START_LIFE); `flashes` is 1..3.
//   life == 2, the first tick:
//       client — the thunder (10000 volume) and impact sounds;
//       server — spawnFire(4) on NORMAL/HARD difficulty.
//   --life each tick. Once below zero: with no flashes left the bolt
//   discards; otherwise, after a random 0..9 extra ticks, it re-flashes
//   (life = 1, a NEW seed so the bolt redraws in a new shape, spawnFire(0)).
//   While life >= 0:
//       client — ClientLevel.setSkyFlashTime(2) (EntityLevel::SetSkyFlashTime);
//       server — unless visual-only, every living entity in the box
//                (±3 horizontally, -3..+9 vertically) is thunderHit.
//
// Both sides run this tick independently from their own entity random — MC
// does not sync the seed or the flash count, and neither does this.
//
// Not modelled, each named at its site in the .cpp:
//   * powerLightningRod (LightningRodBlock.onLightningStrike) and
//     clearCopperOnLightningStrike (the copper de-oxidation walk);
//   * the per-type thunderHit overrides — creeper powering, pig → zombified
//     piglin, villager → witch, mooshroom recolour, turtle bowl drop, copper
//     golem de-oxidation, armor stand / cushion handling (the hanging
//     entities' — paintings, item frames — is: their thunderHit does nothing).
//     Every entity gets Entity.thunderHit's base behaviour (ThunderHit below);
//   * the LIGHTNING_STRIKE / CHANNELED_LIGHTNING advancement triggers, the
//     `cause` player (trident channeling) and the hitEntities set that only
//     feeds them, and gameEvent(LIGHTNING_STRIKE) (no sculk);
//   * a lightning damage type: the 5 damage lands as MobDamageSource::Generic
//     with no attacker — the same no-knockback, no-attacker hit MC's
//     lightning_bolt type gives (it is in #no_knockback) — but the death
//     message reads as a generic death.
#pragma once

#include "common/core/JavaRandom.hpp"
#include "common/entity/projectile/Projectile.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace Game {

    class Entity;

    class LightningBolt : public Projectile {
    public:
        static constexpr int    kStartLife       = 2;     // MC START_LIFE
        static constexpr double kDamageRadius    = 3.0;   // MC DAMAGE_RADIUS
        static constexpr double kDetectionRadius = 15.0;  // MC DETECTION_RADIUS (advancements only)
        static constexpr float  kThunderDamage   = 5.0f;  // MC Entity.thunderHit

        explicit LightningBolt(EntityLevel* level);

        // MC setVisualOnly — no fire, no damage; only the look and the sound.
        void SetVisualOnly(bool visualOnly) { m_visualOnly = visualOnly; }
        bool IsVisualOnly() const { return m_visualOnly; }

        // MC `public long seed` — what LightningBoltRenderer shapes the bolt
        // from. Re-rolled on every re-flash, so each flash has a new shape.
        int64_t GetSeed() const { return m_seed; }

        void Tick() override;

        // MC getBlocksSetOnFire.
        int GetBlocksSetOnFire() const { return m_blocksSetOnFire; }

        // MC Entity.thunderHit — the base behaviour, applied to `victim`:
        // one more fire tick, a full 8-second ignition when that lands on
        // MC's "not burning" resting value, then 5 lightning damage. Public
        // because Twilight Forest's portal (TFPortalBlock.causeLightning)
        // strikes the entities around the pool itself.
        static void ThunderHit(Entity& victim);

        // MC EntityType.LIGHTNING_BOLT is noSave(): a bolt never reaches disk.
        bool CanSerialize() const override { return false; }
        // MC Entity.isPickable defaults false; the bolt never overrides it.
        bool IsPickable() const override { return false; }
        // MC LightningBolt.hurtServer returns false (Projectile refuses Hurt
        // too); nothing may target it either.
        bool IsAttackable() const override { return false; }

        // MC SummonCommand runs finalizeSpawn only for a Mob — the bolt is
        // not one, so the Mob base's spawn rolls must not touch it.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason, std::shared_ptr<SpawnGroupData> groupData) override {
            return groupData;
        }

    private:
        // MC spawnFire(additionalSources).
        void SpawnFire(int additionalSources);

        // MC Entity.random — the bolt's own stream, seeded from the level's
        // on construction (MC seeds every entity's random the same way).
        JavaRandom m_random{0};

        int     m_life = kStartLife;
        int64_t m_seed = 0;
        int     m_flashes = 0;
        bool    m_visualOnly = false;
        int     m_blocksSetOnFire = 0;
    };

} // namespace Game
