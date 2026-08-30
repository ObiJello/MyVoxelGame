// File: src/common/world/level/Explosion.hpp
//
// MC net.minecraft.world.level.ServerExplosion — the one explosion in the
// engine, shared by TNT, creepers, the wither's spawn burst, ghast fireballs
// and wither skulls.
//
// It replaces two near-duplicates that had drifted apart: the entity-only
// `ExplosionHurtEntities` in Monsters.cpp (which had the exposure raycast and
// the right damage curve but destroyed no blocks) and
// `Projectile::ExplodeDamageOnly` (which had NO raycast at all, so a fireball
// dealt full damage through a wall).
//
// ── The block scan, and why it produces a ragged crater ──────────────────
//
// MC does not test a sphere. It fires 1352 rays — every cell on the SHELL of a
// 16x16x16 grid — outward from the centre, each starting with a randomised
// power of `radius * (0.7 + rand * 0.6)`. Each ray marches in 0.3-block steps,
// losing a flat 0.225 per step plus `(resistance + 0.3) * 0.3` for whatever it
// passes through, and marks every cell it reaches while it still has power.
//
// The per-ray randomisation is what makes a TNT crater asymmetric rather than
// a clean hemisphere, and the flat per-step loss is what caps the reach at
// roughly the radius even through air. Both constants are exact and neither is
// tunable without the crater stopping looking like Minecraft.
#pragma once

#include "common/physics/Physics.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/world/block/BlockState.hpp"

#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    class  Entity;
    struct EntityLevel;
    struct ExplosionParams;

    // MC Level.ExplosionInteraction — what KIND of explosion this is. The
    // level maps it onto a BlockInteraction using the drop-decay gamerules,
    // which is why the caller says "I am TNT" rather than "destroy with decay".
    enum class ExplosionInteraction : uint8_t {
        None,     // no block interaction at all
        Block,    // a block did it (bed, respawn anchor)
        Mob,      // a creeper, a wither — gated on mobGriefing
        Tnt,
        Trigger,  // wind charges: triggers blocks, breaks nothing
    };

    // MC Explosion.BlockInteraction — the resolved answer.
    enum class ExplosionBlockInteraction : uint8_t {
        Keep,               // touch no blocks
        Destroy,            // break them, full drops
        DestroyWithDecay,   // break them, each item survives at 1/radius
        TriggerBlock,       // fire block triggers, break nothing
    };

    // MC ExplosionDamageCalculator — the per-explosion policy object. Most
    // blasts use the base class (every method a constant), but three do not:
    //
    //   * WitherSkull / the wither's own blast override shouldBlockExplode to
    //     spare bedrock-class blocks it otherwise could not reach;
    //   * the wind charges override getKnockbackMultiplier and turn damage off;
    //   * an explosion that came through a portal spares the portal blocks.
    //
    // Modelled as a small struct of optional overrides rather than a virtual
    // class so ExplosionParams stays an aggregate the callers fill in place.
    struct ExplosionDamageCalculator {
        // MC getBlockExplosionResistance override. Return false to fall
        // through to the default (max of block and fluid resistance).
        bool (*blockResistance)(const ExplosionParams& p, const glm::ivec3& pos,
                                BlockState state, float& out) = nullptr;

        // MC shouldBlockExplode — false spares the block entirely.
        bool (*shouldBlockExplode)(const ExplosionParams& p, const glm::ivec3& pos,
                                   BlockState state, float power) = nullptr;

        // MC shouldDamageEntity — false means push without hurting.
        bool (*shouldDamageEntity)(const ExplosionParams& p, const Entity& e) = nullptr;
    };

    struct ExplosionParams {
        glm::dvec3 center{0.0};
        float      radius = 4.0f;

        // The entity that IS the explosion (the TNT, the creeper). Excluded
        // from its own entity sweep, and used for MC's PrimedTnt origin quirk.
        Entity* source = nullptr;
        // Who gets the kill credit — a TNT's owner rather than the TNT.
        Entity* attributedTo = nullptr;

        ExplosionInteraction interaction = ExplosionInteraction::None;
        bool  fire = false;

        // False for the wind charges, which push without hurting.
        bool  damageEntities = true;
        // MC ExplosionDamageCalculator.getKnockbackMultiplier — 1.0 for
        // everything except the wind charges (1.22 player, 1.0 breeze).
        float knockbackMultiplier = 1.0f;

        // MC PrimedTnt.usedPortal / the portal-immune damage calculator. A TNT
        // that travelled through a nether portal must not blow the portal out
        // behind it, so its blast spares NETHER_PORTAL and OBSIDIAN.
        bool  sparePortalBlocks = false;

        // Per-explosion policy overrides; all-null means MC's base calculator.
        ExplosionDamageCalculator calculator{};

        // Broadcast the client-side visual. False only for tests.
        bool  spawnVisual = true;
    };

    struct ExplosionResult {
        int blocksDestroyed = 0;
    };

    // MC ServerExplosion.explode(). A no-op returning zero on the client —
    // vanilla's ClientLevel.explode is literally an empty method, because the
    // blast arrives as a packet rather than being simulated twice.
    ExplosionResult Explode(EntityLevel& level, const ExplosionParams& params);

    // ── The two halves of a blast ──────────────────────────────────────────
    //
    // Explode() above is ExplosionScanCrater() followed by ExplodeApply(). They
    // are also exposed separately because the FIRST half is pure — it reads the
    // block field and writes nothing — and a mass detonation is 54 blasts a
    // tick at 245 us of scan each, which is 54% of the server tick and has no
    // business being on it. ServerLevelBridge::ResolveQueuedExplosions runs the
    // scans across the worker pool and then applies them serially, in order.
    //
    // The number of rays a blast fires, which is also exactly how many RNG
    // draws it consumes: the shell of a 16^3 grid, 16^3 - 14^3.
    inline constexpr int kExplosionRayCount = 16 * 16 * 16 - 14 * 14 * 14;

    // The read-only half: which cells the blast reaches.
    //
    // `jitter` must point at kExplosionRayCount floats ALREADY DRAWN from the
    // level's RNG, in ray order, on the thread that owns it. That indirection
    // is the whole reason this can be parallel — the per-ray power jitter is
    // the only thing in the scan that touches shared mutable state, and
    // JavaRandom is not thread-safe.
    //
    // THIS SHIFTS THE RNG STREAM for a batch of more than one blast, and the
    // shift is not avoidable while the scans run in parallel. Serially the
    // stream reads
    //     [blast 1 jitter][blast 1 drop rolls][blast 2 jitter][blast 2 drops]
    // and batched it reads
    //     [all jitter][blast 1 drops][blast 2 drops]
    // because InteractWithBlocks draws too. Order WITHIN each half is exact and
    // the whole thing is deterministic, so a given seed still replays; it is
    // simply not the same sequence a one-at-a-time detonation would produce.
    // Crater raggedness and drop rolls differ. Nothing else reads this stream.
    //
    // Passing nullptr draws from level.Random() inline, which is what the
    // single-blast Explode() path does — that path is unchanged in every
    // respect, so a lone creeper or fireball still matches vanilla exactly.
    std::vector<glm::ivec3> ExplosionScanCrater(EntityLevel& level,
                                                const ExplosionParams& params,
                                                const float* jitter);

    // Everything the blast MUTATES, given the crater the scan produced: the
    // entity sweep, the block breaking, the fire, the sound and the broadcast.
    // Must run on the tick thread, and for a batch must run in the same order
    // the blasts were queued — every Hurt, every AddDeltaMovement and every RNG
    // draw they reach depends on it.
    ExplosionResult ExplodeApply(EntityLevel& level, const ExplosionParams& params,
                                 std::vector<glm::ivec3>& toBlow);

    // A whole tick's blasts at once. Semantically ExplodeApply over each in
    // order, restructured so the cost is O(victims + blasts) per cluster of
    // nearby blasts instead of O(victims x blasts):
    //
    //   * blasts are clustered by 16-block cell; each cluster gathers its
    //     victims ONCE and builds ONE occlusion grid;
    //   * exposure is tabulated per (blast, victim cell) in parallel;
    //   * push-only victims (primed TNT — see Entity::ExplosionPushOnly)
    //     accumulate every blast's knockback in parallel, one entity per
    //     task, blasts in queue order;
    //   * everything that damages (players, mobs) keeps the serial in-order
    //     path with its own exact per-victim exposure;
    //   * the block breaking, fire, sound and broadcast then run serially in
    //     queue order, so the RNG stream is what the per-blast apply drew.
    //
    // DIVERGENCES, all batch-only: every blast's exposure reads the field as
    // it stood before any of this tick's blasts broke a block (the serial
    // apply let blast N see blast N-1's crater); a victim's knockback is the
    // rounded sum of its impulses rather than a running sum (same order,
    // different rounding); push-only victims share their cell's exposure.
    //
    // Returns the nanoseconds spent on the count-independent part (the
    // victim gather and binning), so the caller's budget can separate it
    // from the per-blast cost.
    // `outBlastNs`, when non-null, receives the nanoseconds of the truly
    // blast-proportional part (the block/fire/broadcast tail); the return is
    // the count-independent remainder. The caller's admission gate divides
    // only the former by the blast count — measuring the marginal cost by
    // subtraction under-admitted by two orders of magnitude at a million
    // victims, because the victim-scaled work swamped the difference.
    int64_t ExplodeApplyBatch(EntityLevel& level, std::vector<ExplosionParams>& params,
                              std::vector<std::vector<glm::ivec3>>& craters,
                              int64_t* outBlastNs = nullptr);

    // MC ServerExplosion.getSeenPercent — the fraction of a grid of sample
    // points on the victim's bounding box with clear line of sight to the
    // centre. This is the "hiding behind a wall works" term, and it is the
    // piece Projectile::ExplodeDamageOnly was missing.
    //
    // Exposed because the entity half is useful on its own and is unit-testable
    // without a world full of blocks.
    float ExplosionSeenPercent(const EntityLevel& level, const glm::dvec3& center,
                               const Entity& entity,
                               const CollisionGrid* occlusion = nullptr);

    // Same, reporting the raw ray tallies. Diagnostic only — a bare percentage
    // cannot distinguish "6 of 45 rays slipped past a corner" from "the sample
    // grid collapsed to 6 rays", and those want opposite fixes.
    float ExplosionSeenPercentDetailed(const EntityLevel& level, const glm::dvec3& center,
                                       const Entity& entity, int& outHits, int& outTotal,
                                       const CollisionGrid* occlusion = nullptr);

    // Same, for a victim that is not a Game::Entity. Dropped items and XP orbs
    // live in their own managers here (they are plain structs, not entities),
    // so the blast reaches them through this and ApplyExplosionToLooseEntities
    // rather than through the entity sweep.
    float ExplosionSeenPercentBox(const EntityLevel& level, const glm::dvec3& center,
                                  const AABBd& box,
                                  const CollisionGrid* occlusion = nullptr);

    // What a blast does to one victim, computed from its box alone. Used by
    // EntityLevel::ApplyExplosionToLooseEntities so the loose-entity managers
    // do not each re-derive MC's damage curve and knockback.
    struct ExplosionImpact {
        bool       inRange   = false;
        float      damage    = 0.0f;
        glm::dvec3 knockback{0.0};
    };
    ExplosionImpact ComputeExplosionImpact(const EntityLevel& level,
                                           const ExplosionParams& p,
                                           const glm::dvec3& entityPos,
                                           const AABBd& box,
                                           const CollisionGrid* occlusion = nullptr);

    // MC ExplosionDamageCalculator.getEntityDamageAmount:
    //     pow = (1 - dist) * exposure
    //     damage = (pow^2 + pow) / 2 * 7 * (2 * radius) + 1
    // where `dist` is the distance to the victim over TWICE the radius.
    float ExplosionDamageAmount(float radius, double distanceNormalised, float exposure);

    // MC BlockBehaviour.onExplosionHit's `block.dropFromExplosion(explosion)`.
    // False means "breaks but drops nothing" — TNT is the only member, because
    // a chain detonation already re-spawns it as a primed entity and dropping
    // the item as well would duplicate it.
    bool BlockDropsFromExplosion(BlockID id);

} // namespace Game
