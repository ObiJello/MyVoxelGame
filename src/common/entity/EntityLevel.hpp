// File: src/common/entity/EntityLevel.hpp
//
// The seam between the ported entity system and this engine's world.
//
// MC's entities talk to `Level`, which is enormous and exists in two flavours
// (ServerLevel / ClientLevel). Rather than drag that in, the port talks to this
// narrow interface — everything Entity, LivingEntity, Mob, the goals, the
// navigation and the spawner actually need, and nothing else. Server and client
// each provide one implementation.
//
// Keeping it explicit has a second benefit: every place the port had to adapt
// to an engine that is not Minecraft is visible here as a named method with a
// comment, instead of being buried in a mob class.
#pragma once

#include "common/core/Uuid.hpp"

#include "common/entity/Attributes.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockState.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include "common/world/level/DimensionId.hpp"

namespace Game {

    struct CollisionGrid;

    struct ExplosionParams;

    class ILevelWrite;

    struct IBlockAccess;
    class Entity;
    class LivingEntity;
    class JavaRandom;
    class BaseContainerBlockEntity;
    class IDragonFight;

    // MC net.minecraft.world.Difficulty. Nothing in this engine set a
    // difficulty before mobs existed, so the level implementations default to
    // Normal; PEACEFUL is still honoured everywhere MC honours it, so wiring a
    // setting to it later is a one-line change.
    enum class Difficulty : uint8_t {
        Peaceful = 0,
        Easy,
        Normal,
        Hard,
    };

    // MC DifficultyInstance, reduced to the one number mobs read. MC scales it
    // by chunk inhabited time and moon phase; without those this is a plain
    // function of the difficulty setting. The values are DELIBERATELY the
    // fully-inhabited steady state (what MC converges to where players live
    // and build farms), not the fresh-world floor — a brand-new MC world
    // reads 0.0 on Normal and 0.125 on Hard, ramping up as chunks accrue
    // inhabited time. Implementing per-chunk inhabited time is the honest
    // fix if the ramp ever matters.
    inline float GetSpecialMultiplier(Difficulty d) {
        switch (d) {
            case Difficulty::Peaceful: return 0.0f;
            case Difficulty::Easy:     return 0.0f;
            case Difficulty::Normal:   return 0.5f;
            case Difficulty::Hard:     return 1.0f;
        }
        return 0.5f;
    }

    // ── Particles the entity system emits ──────────────────────────────────
    //
    // The closed set of MC particle types mob code actually asks for. Each
    // entry names the MC ParticleTypes id it stands for; the client's
    // MobParticleSystem owns the per-type physics/sprites (ported from
    // $MC/client/particle/*.java).
    enum class ParticleKind : uint8_t {
        Heart,            // ParticleTypes.HEART          (HeartParticle)
        AngryVillager,    // ParticleTypes.ANGRY_VILLAGER (HeartParticle.AngryVillagerProvider)
        Smoke,            // ParticleTypes.SMOKE          (SmokeParticle, generic_* sheet)
        LargeSmoke,       // ParticleTypes.LARGE_SMOKE    (LargeSmokeParticle, 2.5x scale)
        Poof,             // ParticleTypes.POOF           (ExplodeParticle)
        Explosion,        // ParticleTypes.EXPLOSION      (HugeExplosionParticle; vx = MC's `size` arg)
        ExplosionEmitter, // ParticleTypes.EXPLOSION_EMITTER (HugeExplosionSeedParticle — spawns 6 EXPLOSION/tick for 8 ticks)
        EntityEffect,     // ParticleTypes.ENTITY_EFFECT  (SpellParticle; colour via AddColorParticle)
        WitchMagic,       // ParticleTypes.WITCH          (SpellParticle.WitchProvider — purple, G=0)
        // ParticleTypes.FALLING_DUST (FallingDustParticle). The trickle under
        // an unsupported sand or gravel block. Uses the SAME generic_* sheet
        // Smoke and Poof do — that is MC's own choice, not a stand-in — tinted
        // per block through AddColorParticle.
        FallingDust,
    };

    // MC client ParticleStatus (Options "particles"): the ordinals are the
    // numbers options.txt stores. Lives in common so the explosion debris
    // spawner — client-only logic that runs from common entity code — can
    // ask the level for it the way ClientExplosionTracker reads Options.
    enum class ParticleStatus : uint8_t {
        All       = 0,
        Decreased = 1,
        Minimal   = 2,
    };

    // Custom entity-event bytes — DEVIATIONS from MC, documented here once.
    //
    // MC carries the explosion VISUAL in ClientboundExplodePacket (center,
    // radius, particle, sound); this port has no explosion packet, so the
    // sources that explode broadcast one of these bytes instead and the
    // client entity spawns the same particles MC's handleExplosion would.
    // MC's own entity-event ids run 0..69 (EntityEvent.java, RAVAGER_ROARED
    // = 69 highest); 100+ is clear of them.
    //
    //   kEntityEventExplosionEmitter / kEntityEventExplosionSmall — RETIRED.
    //       These stood in for MC's ClientboundExplodePacket back when the
    //       engine had no explosion system and no destroyed-block count to
    //       send. There is a real ExplodeS2C packet now, carrying the exact
    //       centre, the real block count and the local player's knockback, so
    //       nothing broadcasts these any more. The constants are kept because
    //       the byte values are wire-visible and re-using 100/101 for a
    //       different meaning would confuse an older client.
    //   kEntityEventLoveHeart — ONE periodic courtship heart. In MC
    //       Animal.aiStep spawns this client-side from the local inLove
    //       counter, which is not synced in this port; the server ticks the
    //       counter, so it broadcasts the cadence instead (1 heart per 10
    //       ticks — the byte replaces MC's client-local addParticle, the
    //       cadence and the particle are MC's).
    //   kEntityEventSwing — the melee arm swing. MC carries this as
    //       ClientboundAnimatePacket(SWING_MAIN_HAND), not an entity event;
    //       this port has no Animate packet, so the byte stands in. The
    //       client handler calls Swing() on its entity copy and the local
    //       swing clock does the rest (LivingEntity::UpdateSwingTime).
    static constexpr uint8_t kEntityEventExplosionEmitter = 100;
    static constexpr uint8_t kEntityEventExplosionSmall   = 101;
    static constexpr uint8_t kEntityEventLoveHeart        = 102;
    static constexpr uint8_t kEntityEventSwing            = 103;

    struct EntityLevel {
        virtual ~EntityLevel() = default;

        // ── World data ─────────────────────────────────────────────────────
        virtual const IBlockAccess* Blocks() const = 0;

        // Monotonic counter bumped by every accepted block write, for readers
        // that cache a view of the block field (see Game::CollisionGrid). The
        // default 0 means "no epoch tracking" and compares equal to itself, so
        // a level that does not implement it simply never invalidates — which
        // is correct for the client, whose bridge does not build such caches.
        virtual uint64_t BlockWriteEpoch() const { return 0; }

        // ── Detonation admission (DELIBERATE DIVERGENCE FROM MINECRAFT) ─────
        //
        // Returns false when this tick has already spent its explosion budget,
        // in which case the caller holds its fuse at zero and asks again next
        // tick. MC has no such gate: PrimedTnt.tick calls explode() inline and
        // a mass detonation simply stalls the server for as long as it takes.
        //
        // Measured: one blast costs ~1,295 us, so a million of them is ~22
        // minutes of CPU no matter how it is scheduled. The choice is only
        // whether that arrives as one unbroken freeze or as a world that keeps
        // running at 20 TPS while the cascade drains. This picks the latter.
        //
        // Gating the FUSE rather than queueing the explosion is what keeps it
        // safe: a queued blast would have to outlive the PrimedTnt that raised
        // it (MC discards the entity before exploding), so its `source` and
        // `attributedTo` pointers would dangle by drain time. Holding the fuse
        // keeps the entity alive and the explosion itself byte-identical.
        //
        // Always grants at least one blast per tick, so a single expensive
        // explosion can never livelock the cascade.
        virtual bool TryBeginExplosion() { return true; }

        // ── Batched detonation ─────────────────────────────────────────────
        //
        // Hand a blast over instead of running it now. The server override
        // collects a tick's worth, runs the READ-ONLY crater scans across the
        // worker pool, then applies them serially in the order they arrived —
        // see ServerLevelBridge::ResolveQueuedExplosions and the note on
        // Game::ExplosionScanCrater.
        //
        // The default explodes immediately, because a level with no batcher
        // (the client mirror, tests) must still behave. `p.source` is READ
        // during the apply, so a caller queueing a blast whose source it is
        // about to destroy must guarantee the source outlives the resolve —
        // PrimedTnt does, because the resolve runs before MobManager's sweep.
        virtual void QueueExplosion(const ExplosionParams& p);

        // Convenience: a PhysicsContext wrapping Blocks(), for the mover.
        PhysicsContext Physics() const {
            PhysicsContext ctx;
            ctx.blockAccess = Blocks();
            return ctx;
        }

        // MC Level.isClientSide. Gates every server-only branch in the port —
        // AI, damage, drops, despawn — so a client-side mob runs travel() and
        // animation only, exactly as in MC.
        virtual bool IsClientSide() const = 0;

        virtual int64_t GetGameTime() const = 0;
        virtual int64_t GetDayTime()  const = 0;
        virtual Difficulty GetDifficulty() const { return Difficulty::Normal; }

        virtual JavaRandom& Random() = 0;

        // ── Light ──────────────────────────────────────────────────────────
        //
        // This engine has NO light engine (see IBlockAccess::GetRawBrightness —
        // it answers 15 for a column open to the sky and 0 for a roofed one).
        // That stand-in is correct for crops, which read raw sky light, but it
        // is not usable for mob spawning: monsters would never spawn on the
        // surface at night, because the raw value never dims.
        //
        // So the port routes every spawn/AI light test through this method,
        // which is MC's getMaxLocalRawBrightness — raw sky light MINUS the
        // time-of-day darkening. Open sky at midnight reads ~4, a cave reads 0,
        // and open sky at noon reads 15, which is the behaviour the rules were
        // written against. Torches still contribute nothing; that arrives with
        // a real light engine and only this method changes.
        virtual int GetMaxLocalRawBrightness(int x, int y, int z) const = 0;

        // MC LevelReader.getBrightness(LightLayer.SKY, pos) — the RAW stored sky
        // light, with no time-of-day darkening applied. Distinct from
        // GetMaxLocalRawBrightness on purpose: Monster.isDarkEnoughToSpawn
        // tests both, and they answer differently. Outdoors this is 15 at
        // midnight as well as at noon.
        virtual int GetSkyBrightness(int x, int y, int z) const = 0;

        // MC Level.getMaxLocalRawBrightness(pos, amount) — the explicit-amount
        // overload, used when thundering (amount = 10) instead of skyDarken.
        virtual int GetMaxLocalRawBrightness(int x, int y, int z, int amount) const = 0;

        // MC Level.getSkyDarken. 0 in full day, 11 at night, interpolated
        // across dawn and dusk — see the implementation for the keyframes.
        virtual int GetSkyDarken() const = 0;

        // MC Level.canSeeSky — needed by the zombie daylight burn and by
        // skeleton sun avoidance.
        virtual bool CanSeeSky(int x, int y, int z) const = 0;

        // MC's EnvironmentAttributes.MONSTERS_BURN timeline track. NOT the same
        // as IsDay(): the burn window is [23460, 12542), narrower than the day
        // at both ends, so undead survive a little past dawn and catch fire a
        // little before dusk.
        virtual bool MonstersBurn() const = 0;

        // MC Level.isDay / isThundering.
        virtual bool IsDay() const = 0;
        virtual bool IsThundering() const { return false; }

        // Biome base temperature at a position — MC Biome.getBaseTemperature,
        // the number the SNOW_GOLEM_MELTS environment attribute compares
        // against 1.0. Server-side only (the snow-golem melt is server AI);
        // the client default is a temperate 0.8.
        virtual float GetBiomeTemperature(int x, int y, int z) const {
            (void)x; (void)y; (void)z;
            return 0.8f;
        }

        // ── Entity queries ─────────────────────────────────────────────────
        //
        // MC's getEntitiesOfClass is generic; here the caller filters by type
        // after the fact, because the port has a closed set of mob classes and
        // a template-per-class query would buy nothing.
        //
        // `except` is skipped. Results are NOT sorted.
        virtual void GetEntitiesInBox(const AABB& box, const Entity* except,
                                      std::vector<Entity*>& out) const = 0;

        // MC Level.getNearestPlayer. Returns null when none is in range;
        // `maxDistance` < 0 means unlimited. Players are exposed as
        // LivingEntity so goals can target and damage them uniformly — see
        // the adapter note in ServerLevelBridge.
        virtual LivingEntity* GetNearestPlayer(double x, double y, double z,
                                               double maxDistance) const = 0;

        virtual void GetPlayers(std::vector<LivingEntity*>& out) const = 0;

        // ── Identity lookup, for saved cross-entity references ──────────────
        //
        // EntityRef resolves through these. Split the way MC does: entities go
        // through the level (and its siblings, because a reference can cross a
        // dimension — ServerLevel.getEntityInAnyDimension), players through the
        // server-wide player list.
        //
        // Both default to null so the client level and any test double need not
        // implement them; an unresolved reference is a valid state.
        virtual Entity*       ResolveEntity(const Uuid& uuid) const { (void)uuid; return nullptr; }
        virtual LivingEntity* ResolvePlayer(const Uuid& uuid) const { (void)uuid; return nullptr; }

        // The item id in a player's main hand, or 0 for empty.
        //
        // The item system lives outside the entity port (Game::ItemStack,
        // Game::Inventory) and TemptGoal is the only thing that needs to reach
        // it, so it comes through the bridge rather than being a dependency of
        // every mob. Returns 0 for non-players.
        virtual uint32_t GetHeldItemId(const LivingEntity& player) const { return 0; }

        // ── Effects the entity system causes ───────────────────────────────

        // Broadcast a one-byte entity event to everyone tracking `entity`
        // (MC Level.broadcastEntityEvent): 3 = death, 60 = poof particles,
        // 10 = sheep eat, 18 = breeding hearts.
        virtual void BroadcastEntityEvent(const Entity& entity, uint8_t event) = 0;

        // MC Level.addParticle. Live on the CLIENT only — exactly MC's
        // split: Level.addParticle is an empty default overridden by
        // ClientLevel, so entity code calls it unconditionally and the
        // server side is a no-op. ClientLevelBridge overrides this to queue
        // the spawn for the MobParticleSystem.
        virtual void AddParticle(ParticleKind kind, double x, double y, double z,
                                 double vx, double vy, double vz) {
            (void)kind; (void)x; (void)y; (void)z; (void)vx; (void)vy; (void)vz;
        }

        // The client's Particles option. Only the client bridge answers with
        // anything but All; the server never spawns visual particles.
        virtual ParticleStatus GetParticleStatus() const { return ParticleStatus::All; }

        // MC ColorParticleOption.create(ENTITY_EFFECT, r, g, b) — the
        // colour-carrying variant SpellParticle's MobEffectProvider reads.
        // Default forwards without the colour so a server bridge stays a
        // single no-op.
        virtual void AddColorParticle(ParticleKind kind, double x, double y, double z,
                                      double vx, double vy, double vz,
                                      float r, float g, float b, float a) {
            (void)r; (void)g; (void)b; (void)a;
            AddParticle(kind, x, y, z, vx, vy, vz);
        }

        // Drop an item stack in the world. Server-only; the client
        // implementation is a no-op.
        virtual void SpawnItemDrop(const glm::dvec3& pos, uint32_t itemId, int count) {}

        // MC ExperienceOrb.award(level, pos, amount): spawns real orb
        // entities at `pos` (Server::ExperienceOrbManager via
        // ServerLevelBridge::AwardExperience) that fly to whichever player
        // comes within follow range. `creditPlayerEntityId` is kept for the
        // call sites' documentation value (a kill's crediting player, a
        // breeding's feeder) but no longer picks the recipient — the orbs
        // do, exactly as in MC. Server-only; the client implementation is a
        // no-op.
        virtual void AwardExperience(const glm::dvec3& pos, int amount,
                                     int32_t creditPlayerEntityId) {
            (void)pos; (void)amount; (void)creditPlayerEntityId;
        }

        // Add a freshly created entity to the level, taking ownership.
        // Server-only (the spawner, zombie reinforcements, breeding, arrows).
        virtual void AddFreshEntity(std::unique_ptr<Entity> entity) {}

        // ── Block edits made BY mobs ────────────────────────────────────────
        //
        // MC Level.destroyBlock(pos, dropResources) and Level.setBlock. A sheep
        // grazing is the first user; endermen and creepers want the same pair.
        //
        // Both are gated by the caller on MobGriefing() — MC's `mobGriefing`
        // gamerule — rather than being gated in here, because MC checks it at
        // the call site and the decision belongs to whatever is doing the
        // griefing, not to the level.
        //
        // Server-only; the client implementations are no-ops, which is right
        // because client mobs never run AI.
        virtual void DestroyBlock(const glm::ivec3& pos, bool dropResources) {}
        virtual void SetBlock(const glm::ivec3& pos, BlockID block) {}

        // MC Level.setBlock(pos, STATE, 2) — the state-aware setter mob code
        // needs when the write is a PROPERTY change rather than a block swap:
        // the rabbit knocking a carrot crop down an age (RaidGardenGoal), the
        // bee growing one up (BeeGrowCropGoal). Flag 2 (update clients, no
        // neighbour shape updates) is MC's for both call sites and is what the
        // server bridge forwards.
        virtual void SetBlockState(const glm::ivec3& pos, BlockState state) {}

        // The WRITABLE block view, for entity code that needs the flagged
        // SetBlock rather than the three narrow helpers above — a falling block
        // placing itself has to pass MC's flag 3 (neighbours + clients) so the
        // sand above it learns to fall next.
        //
        // NULL on the client, and that is the point rather than an omission: a
        // falling block's landing is server authority, and letting the client's
        // predicted world place it would produce a block the server never
        // agreed to and then rewind it. MC says the same thing by gating the
        // whole landing branch behind `!level.isClientSide()`.
        virtual ILevelWrite* MutableBlocks() { return nullptr; }

        // Which dimension this level IS — MC Level.dimension(). The default
        // is the Overworld; ServerLevelBridge answers from its World. Drop
        // helpers need it because "spawn an item at (0, 70, 0)" cannot name a
        // world from the numbers alone, and the shared drop functions used to
        // assume the Overworld — which is how an item dropped in the End
        // landed a dimension away.
        virtual DimensionId Dimension() const { return DimensionId::Overworld; }

        // MC ServerLevel.getDragonFight() — non-null only on the server-side
        // End level. The dragon reads crystal counts and reports its health
        // through this; the crystal reports its destruction. See
        // common/entity/DragonFight.hpp for why the controller itself is
        // server-side.
        virtual IDragonFight* DragonFight() { return nullptr; }

        // MC GameRules.RULE_MOBGRIEFING. Default true, as in vanilla.
        virtual bool MobGriefing() const { return true; }

        // MC GameRules.RULE_DOMOBSPAWNING — read by the ender pearl's 5%
        // endermite roll (the natural spawner reads the World's copy
        // directly). Default true, as in vanilla.
        virtual bool DoMobSpawning() const { return true; }

        // Teleport a PLAYER to `pos`, keeping their look direction. Player
        // movement is client-authoritative in this engine, so the move must
        // be SENT (ServerConnection::Teleport) rather than written to the
        // mirror view, whose position would be overwritten by the next move
        // packet. Server bridge only; false anywhere else, and false for an
        // entity that is not actually a player. First user: the thrown ender
        // pearl (MC ServerPlayer.teleport via TeleportTransition).
        virtual bool TeleportPlayer(LivingEntity& player, const glm::dvec3& pos) {
            (void)player; (void)pos;
            return false;
        }

        // MC Level.getMinY() / getMaxY() — the DIMENSION's build limits, which
        // are not the same as this engine's fixed chunk storage range. The
        // nether is 0..127 and the end 0..255, so a falling block that leaves
        // the End island must despawn 64 blocks earlier than the overworld
        // constants would say. Defaults are the overworld's; ServerLevelBridge
        // resolves them from the world's DimensionId.
        virtual int GetMinY() const { return -64; }
        virtual int GetMaxY() const { return 319; }

        // ── Explosion gamerules ────────────────────────────────────────────
        //
        // Mirrored here so common/entity and the explosion module never have
        // to include World.hpp. Defaults are MC's, and the client's bridge
        // keeps them — a client evaluating an explosion visual has no gamerule
        // state and does not need one.
        virtual bool TntExplodes() const { return true; }
        virtual bool DoEntityDrops() const { return true; }
        // MC's tnt_explosion_drop_decay defaults FALSE — vanilla TNT drops
        // everything it breaks. See World.hpp.
        virtual bool TntExplosionDropDecay() const { return false; }
        virtual bool BlockExplosionDropDecay() const { return true; }
        virtual bool MobExplosionDropDecay() const { return true; }

        // ── Explosion callbacks ────────────────────────────────────────────
        //
        // MC TntBlock.wasExploded — a TNT block caught in a blast re-primes
        // with a SHORT random fuse instead of dropping, which is the whole
        // chain-detonation mechanism. It lives behind a virtual because
        // Explosion.cpp is common code and spawning is server authority.
        virtual void OnTntExploded(const glm::ivec3& pos, Entity* igniter) {
            (void)pos; (void)igniter;
        }

        // MC ServerExplosion.hurtEntities reaches EVERY entity in range. This
        // engine keeps two kinds outside the Game::Entity hierarchy — dropped
        // items and XP orbs are plain structs owned by their own managers — so
        // the entity sweep above cannot see them and a blast used to leave a
        // pile of items sitting untouched in the middle of its own crater.
        //
        // The server's bridge walks those managers and applies
        // Game::ComputeExplosionImpact to each. A no-op on the client, which
        // never simulates a blast.
        // `occlusion` is the blast's shared collision snapshot (may be null);
        // forward it to ComputeExplosionImpact so items and orbs get the same
        // acceleration the entity sweep does. A world that has been blown up a
        // lot is exactly a world full of dropped items, so this is not a
        // marginal population.
        virtual void ApplyExplosionToLooseEntities(const ExplosionParams& params,
                                                   const CollisionGrid* occlusion) {
            (void)params; (void)occlusion;
        }

        // Same, for several blasts against one pass over the loose entities.
        // The default just loops; ServerLevelBridge overrides with a single
        // walk of each manager so a batch of K blasts is O(items + K) rather
        // than O(items * K).
        virtual void ApplyExplosionsToLooseEntities(const ExplosionParams* const* params,
                                                    size_t count,
                                                    const CollisionGrid* occlusion) {
            for (size_t i = 0; i < count; ++i) ApplyExplosionToLooseEntities(*params[i], occlusion);
        }

        // MC ClientboundExplodePacket — tell watching clients to draw the
        // blast. `blockCount` drives the debris particle count and `small`
        // picks EXPLOSION vs EXPLOSION_EMITTER (MC Explosion.isSmall).
        virtual void BroadcastExplosion(const glm::dvec3& center, float radius,
                                        int blockCount, bool small) {
            (void)center; (void)radius; (void)blockCount; (void)small;
        }

        // ── Container block entities read BY mobs ──────────────────────────
        //
        // MC Level.getBlockEntity plus the per-chunk getBlockEntities() walk in
        // TransportItemsBetweenContainers.getTransportTarget, narrowed to the
        // container BEs mob code touches (the copper golem's chest transport).
        //
        // Server-only; the client implementations return nothing, which is
        // right because client mobs never run AI.
        virtual BaseContainerBlockEntity*
        GetContainerBlockEntity(const glm::ivec3& pos) const {
            (void)pos;
            return nullptr;
        }

        // Every container block entity in the LOADED chunks within `chunkRange`
        // chunks of `center` — MC's ChunkPos.rangeClosed(pos, floorDiv(dist,
        // 16) + 1) over getChunkNow, which skips unloaded chunks the same way.
        // Appends to `out`.
        virtual void GetContainerBlockEntities(const glm::ivec3& center, int chunkRange,
                                               std::vector<BaseContainerBlockEntity*>& out) const {
            (void)center; (void)chunkRange; (void)out;
        }
    };

    // MC LevelReader.getLightLevelDependentMagicValue(pos): brightness/15 run
    // through the curve v/(4-3v), lerped toward 1 by the dimension's ambient
    // light (0 in the overworld, so no lerp here). This 0..1 value — NOT the
    // raw 0..15 light — is what MC's "brighter than half" tests compare
    // against 0.5F: the curve crosses 0.5 at brightness 12, so `magic > 0.5`
    // means brightness >= 13, not >= 8.
    inline float LightLevelDependentMagicValue(const EntityLevel& level,
                                               int x, int y, int z) {
        const float v = static_cast<float>(level.GetMaxLocalRawBrightness(x, y, z)) / 15.0f;
        return v / (4.0f - 3.0f * v);
    }

    // MC LevelReader.getPathfindingCostFromLightLevels — the walk-target base:
    // negative in the dark, positive in bright light, zero at brightness 12.
    inline float PathfindingCostFromLightLevels(const EntityLevel& level,
                                                int x, int y, int z) {
        return LightLevelDependentMagicValue(level, x, y, z) - 0.5f;
    }

} // namespace Game
