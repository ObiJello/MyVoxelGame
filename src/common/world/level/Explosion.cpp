// File: src/common/world/level/Explosion.cpp
#include "common/world/level/Explosion.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/SoundEvents.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/ExplosionTrigger.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/loot/LootTables.hpp"
#include "common/entity/Item.hpp"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <map>
#include <cmath>
#include <utility>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/core/TickParallel.hpp"

namespace Game {

    namespace {

        // ── MC ServerExplosion's ray-march constants ────────────────────────
        //
        // Every one of these is exact. Changing any of them stops the crater
        // looking like Minecraft's: the shell size sets the ray count, the
        // power jitter is what makes the crater ragged instead of spherical,
        // and the flat per-step loss is what bounds the reach through open air.
        constexpr int    kGridSize      = 16;
        constexpr float  kStepSize      = 0.3f;
        constexpr float  kPowerPerStep  = 0.22500001f;   // MC's literal, to the digit
        constexpr float  kPowerJitterLo = 0.7f;
        constexpr float  kPowerJitterHi = 0.6f;          // lo + rand * hi
        constexpr float  kResistanceBias = 0.3f;
        // MC ServerExplosion.MAX_DROPS_PER_COMBINED_STACK.
        constexpr int    kMaxDropsPerStack = 16;
        // MC Explosion.isSmall(): radius < 2 (or no block interaction).
        constexpr float  kLargeExplosionRadius = 2.0f;

        int64_t PackPos(const glm::ivec3& p) {
            // 26/12/26 bits, the same packing MC's BlockPos.asLong uses. Ample
            // for a blast, which never spans more than ~32 blocks.
            return (static_cast<int64_t>(p.x) & 0x3FFFFFF) |
                   ((static_cast<int64_t>(p.z) & 0x3FFFFFF) << 26) |
                   ((static_cast<int64_t>(p.y) & 0xFFF) << 52);
        }
        glm::ivec3 UnpackPos(int64_t k) {
            auto sign = [](int64_t v, int bits) {
                const int64_t m = int64_t(1) << (bits - 1);
                return static_cast<int>((v ^ m) - m);
            };
            return glm::ivec3(sign(k & 0x3FFFFFF, 26),
                              sign((k >> 52) & 0xFFF, 12),
                              sign((k >> 26) & 0x3FFFFFF, 26));
        }

        // MC ServerExplosion's constructor mapping, via ServerLevel.explode.
        ExplosionBlockInteraction ResolveInteraction(const EntityLevel& level,
                                                     ExplosionInteraction kind) {
            const auto decay = [](bool on) {
                return on ? ExplosionBlockInteraction::DestroyWithDecay
                          : ExplosionBlockInteraction::Destroy;
            };
            switch (kind) {
                case ExplosionInteraction::None:    return ExplosionBlockInteraction::Keep;
                case ExplosionInteraction::Block:   return decay(level.BlockExplosionDropDecay());
                case ExplosionInteraction::Mob:
                    // A creeper in a world with mobGriefing off damages you and
                    // leaves the terrain alone. That is the whole point of the
                    // rule and it is checked HERE, not at the call site.
                    return level.MobGriefing() ? decay(level.MobExplosionDropDecay())
                                               : ExplosionBlockInteraction::Keep;
                case ExplosionInteraction::Tnt:     return decay(level.TntExplosionDropDecay());
                case ExplosionInteraction::Trigger: return ExplosionBlockInteraction::TriggerBlock;
            }
            return ExplosionBlockInteraction::Keep;
        }

        // MC ServerExplosion.interactsWithBlocks(): `blockInteraction != KEEP`.
        //
        // TRIGGER_BLOCK is INCLUDED. It looks like it should not be — a trigger
        // blast breaks nothing — but the exclusion happens one level down, in
        // BlockBehaviour.onExplosionHit, which returns immediately for
        // TRIGGER_BLOCK. The blocks that DO answer a trigger (buttons, levers,
        // bells) override onExplosionHit, and skipping the whole pass here
        // meant a wind charge never reached any of them.
        //
        // isSmall() shares this predicate, so a breeze wind charge (radius 3)
        // was also reporting small=true and drawing the wrong particle.
        bool InteractsWithBlocks(ExplosionBlockInteraction bi) {
            return bi != ExplosionBlockInteraction::Keep;
        }

        // MC ExplosionDamageCalculator.getBlockExplosionResistance: nothing at
        // all for air with no fluid, otherwise max(block, fluid).
        //
        // The fluid half is NOT redundant with the block half. Water and lava
        // are blocks here and carry strength(100) of their own, but a
        // WATERLOGGED block is a different block wearing water — a waterlogged
        // oak stair has the stair's 3.0 and the water's 100.0, and MC takes the
        // larger. Reading only the block's number let a blast tear through
        // waterlogged stairs, fences, slabs, and every always-waterlogged plant
        // (kelp, seagrass, sea pickle, coral fan) as if the water were not
        // there.
        float FluidResistanceOf(BlockState state) {
            if (!BlockRegistry::ContainsWater(state)) return 0.0f;
            return BlockRegistry::Get(BlockID::Water).explosionResistance;
        }

        // MC PrimedTnt.USED_PORTAL_DAMAGE_CALCULATOR — a blast that arrived
        // through a portal must not take the portal out behind it, which would
        // strand whatever was following.
        //
        // Only NETHER_PORTAL itself. The obsidian frame needs no help: its own
        // 1200 resistance already survives, and vanilla does not list it.
        bool SparesPortalBlock(BlockID id) {
            return id == BlockID::NetherPortal;
        }

        // Both of these take the state the CALLER already read. The march used
        // to fetch it independently in each — same cell, three source lines
        // apart, with no write between — which was 16,732 of the 34,019 chunk
        // lookups a radius-4 blast makes. MC reads it once per step and passes
        // it into both calculator methods; this now matches.
        bool BlockResistance(const ExplosionParams& p, const IBlockAccess& blocks,
                             const glm::ivec3& pos, BlockState state, float& out) {
            (void)blocks;
            const BlockID id = state.Block();
            if (id == BlockID::Air) return false;

            // MC's portal calculator returns Optional.empty() here, i.e. "no
            // block at all" — so the ray passes through the portal WITHOUT
            // losing power, rather than merely failing to break it.
            if (p.sparePortalBlocks && SparesPortalBlock(id)) return false;

            if (p.calculator.blockResistance &&
                p.calculator.blockResistance(p, pos, state, out)) {
                return true;
            }

            out = std::max(BlockRegistry::Get(id).explosionResistance,
                           FluidResistanceOf(state));
            return true;
        }

        // MC ExplosionDamageCalculator.shouldBlockExplode — true in the base
        // class, overridden by the wither's and the portal-immune calculators.
        bool ShouldBlockExplode(const ExplosionParams& p, const IBlockAccess& blocks,
                                const glm::ivec3& pos, BlockState state, float power) {
            (void)blocks;
            if (p.sparePortalBlocks && SparesPortalBlock(state.Block())) return false;
            if (p.calculator.shouldBlockExplode) {
                return p.calculator.shouldBlockExplode(p, pos, state, power);
            }
            return true;
        }

    } // namespace

    // The shared body behind all three exposure entry points. Defined below;
    // declared here because the public wrappers come first.
    static float SeenPercentOfBox(const EntityLevel& level, const glm::dvec3& center,
                                  const AABBd& bb, int& outHits, int& outTotal,
                                  const CollisionGrid* occlusion);

    bool BlockDropsFromExplosion(BlockID id) {
        // MC TntBlock.dropFromExplosion returns false. Everything else uses
        // Block's default of true.
        return id != BlockID::Tnt;
    }

    float ExplosionSeenPercent(const EntityLevel& level, const glm::dvec3& center,
                               const Entity& entity, const CollisionGrid* occlusion) {
        int hits = 0, total = 0;
        return ExplosionSeenPercentDetailed(level, center, entity, hits, total, occlusion);
    }

    float ExplosionSeenPercentBox(const EntityLevel& level, const glm::dvec3& center,
                                  const AABBd& box, const CollisionGrid* occlusion) {
        int hits = 0, total = 0;
        return SeenPercentOfBox(level, center, box, hits, total, occlusion);
    }

    float ExplosionSeenPercentDetailed(const EntityLevel& level, const glm::dvec3& center,
                                       const Entity& entity, int& outHits, int& outTotal,
                                       const CollisionGrid* occlusion) {
        return SeenPercentOfBox(level, center, entity.GetAABBd(), outHits, outTotal, occlusion);
    }

    static float SeenPercentOfBox(const EntityLevel& level, const glm::dvec3& center,
                                  const AABBd& bb, int& outHits, int& outTotal,
                                  const CollisionGrid* occlusion) {
        // Called once per entity per blast — the term that grows quadratically
        // when many entities are in range of each other. Ten million calls in a
        // 10,000-TNT capture, so the ZoneScopedN pair itself was worth hundreds
        // of ms attributed to the function it was measuring. Behind
        // -DEXPLOSION_DETAIL_ZONES=ON; the enclosing HurtEntities and
        // ExposureParallel zones give the same total without the per-call cost.
        PROFILE_ZONE_DETAIL("Explosion.SeenPercent");
        outHits = 0;
        outTotal = 0;
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return 1.0f;

        // The per-ray test is Game::TraverseBlocks (common/world/level/
        // BlockClip.hpp) — MC's BlockGetter.traverseBlocks, an EXACT voxel walk
        // (Amanatides-Woo) visiting every cell the segment passes through and
        // no others.
        //
        // This replaced a 0.25-block point sampler, and the difference is not
        // academic. A sampler tests discrete points, so a ray that only clips a
        // corner of a block — or threads the shared edge between two
        // diagonally-placed blocks — can land every one of its samples in open
        // air and report a clear line of sight. That is exactly what let a TNT
        // sitting in the open corner column of an obsidian enclosure knock the
        // player inside it about: each wall was solid, but the diagonal seam
        // between two of them was invisible to the sampler. MC's traversal
        // cannot miss a cell, so vanilla blocks those rays and does not.
        //
        // The per-cell test is ClipContext.Block.COLLIDER: clip against the
        // block's actual COLLISION SHAPE, not against the cell. That is what
        // lets a ray pass over a slab, under a fence's arm or through the gap
        // in a wall — and what makes those give partial cover rather than
        // full cover. getSeenPercent passes Fluid.NONE, so fluids never block.
        // A grid whose snapshot predates a block write is not usable. One
        // integer compare per exposure call (~1,000 per blast, NOT per ray)
        // turns what would be a silent wrong answer into a silent slow path.
        if (occlusion && occlusion->writeEpoch != level.BlockWriteEpoch()) {
            occlusion = nullptr;
        }

        glm::ivec3 hitCell;
        const auto rayClear = [&](const glm::dvec3& from) {
            return !ClipBlocksCollider(*blocks, from, center, hitCell,
                                       /*includeWaterSource=*/false, occlusion);
        };

        // MC's sample grid: 2*size+1 per axis, so a bigger victim is sampled
        // more finely and a player behind a corner gets partial cover rather
        // than an all-or-nothing verdict.
        // MC's AABB is double throughout. This used to take the FLOAT box and
        // widen it, so every one of the 27 sample origins already differed from
        // vanilla in the low bits, with the error growing with |coordinate|.
        const glm::dvec3 mn(bb.min), mx(bb.max);
        const double xs = 1.0 / ((mx.x - mn.x) * 2.0 + 1.0);
        const double ys = 1.0 / ((mx.y - mn.y) * 2.0 + 1.0);
        const double zs = 1.0 / ((mx.z - mn.z) * 2.0 + 1.0);
        if (xs < 0.0 || ys < 0.0 || zs < 0.0) return 0.0f;
        const double xOffset = (1.0 - std::floor(1.0 / xs) * xs) / 2.0;
        const double zOffset = (1.0 - std::floor(1.0 / zs) * zs) / 2.0;

        // ── Clear-hull early-out ────────────────────────────────────────
        // Every sample origin lies in a box we can bound analytically, and
        // every ray ends at `center`. TraverseBlocks only ever visits cells
        // componentwise within [floor(a), floor(b)] of the nudged endpoints, so
        // if no cell in that box (expanded by one, to absorb the 1e-7 endpoint
        // nudge and FP drift in the tMax accumulation) is collidable, then
        // every one of these rays is clear and the answer is exactly 1.0.
        //
        // Bounded analytically rather than by enumerating the samples: the
        // count is 2*size+1 per axis, which is 27 for a TNT but 18,513 for an
        // ender dragon.
        //
        // The loop below still RUNS — it is ~27 iterations of scalar float
        // against 27 voxel walks — so hits, count, outHits and outTotal are
        // produced by literally the same statements either way.
        bool hullClear = false;
        if (occlusion) {
            // Whole test in double; float this to ints only after, because
            // static_cast<int>(std::floor(x)) on NaN or an out-of-range value
            // is UB. A NaN box reaches here (NaN < 0.0 is false, so the guard
            // above lets it through); NaN comparisons are false, so lo/hi
            // containment fails and we fall through to full raycasting.
            // sample = mn + t*(mx-mn) + offset, t in [0,1], offset in [0,0.5]
            // on x and z and exactly 0 on y (there is no yOffset). Take the
            // interpolation term's range as [min(mn,mx), max(mn,mx)] rather
            // than [mn,mx]: a degenerate box with mx < mn survives the xs<0
            // guard above (when (mx-mn)*2+1 lands in (0,1), xs > 1 and xOffset
            // is exactly 0.5), and assuming mn <= mx there would put the real
            // sample outside the bound.
            const glm::dvec3 tLo = glm::min(mn, mx);
            const glm::dvec3 tHi = glm::max(mn, mx);
            const glm::dvec3 sLo(tLo.x, tLo.y, tLo.z);
            const glm::dvec3 sHi(tHi.x + 0.5, tHi.y, tHi.z + 0.5);
            const glm::dvec3 lo = glm::min(sLo, center);
            const glm::dvec3 hi = glm::max(sHi, center);
            const double kLim = 3.0e7;
            if (lo.x > -kLim && hi.x < kLim && lo.y > -kLim && hi.y < kLim &&
                lo.z > -kLim && hi.z < kLim) {
                const glm::ivec3 cLo(static_cast<int>(std::floor(lo.x)) - 1,
                                     static_cast<int>(std::floor(lo.y)) - 1,
                                     static_cast<int>(std::floor(lo.z)) - 1);
                const glm::ivec3 cHi(static_cast<int>(std::floor(hi.x)) + 1,
                                     static_cast<int>(std::floor(hi.y)) + 1,
                                     static_cast<int>(std::floor(hi.z)) + 1);
                // Only meaningful if the whole hull is inside the grid's real
                // extent — a collidable cell just outside it would be missed,
                // which is exposure 1.0 through a wall.
                if (occlusion->Contains(cLo) && occlusion->Contains(cHi)) {
                    hullClear = !occlusion->AnySetInBox(cLo, cHi);
                }
            }
        }

        int hits = 0, count = 0;
        for (double xx = 0.0; xx <= 1.0; xx += xs) {
            for (double yy = 0.0; yy <= 1.0; yy += ys) {
                for (double zz = 0.0; zz <= 1.0; zz += zs) {
                    const glm::dvec3 from(mn.x + xx * (mx.x - mn.x) + xOffset,
                                          mn.y + yy * (mx.y - mn.y),
                                          mn.z + zz * (mx.z - mn.z) + zOffset);
                    if (hullClear || rayClear(from)) ++hits;
                    ++count;
                }
            }
        }
        outHits = hits;
        outTotal = count;
        return count > 0 ? static_cast<float>(hits) / static_cast<float>(count) : 0.0f;
    }

    ExplosionImpact ComputeExplosionImpact(const EntityLevel& level,
                                           const ExplosionParams& p,
                                           const glm::dvec3& entityPos,
                                           const AABBd& box,
                                           const CollisionGrid* occlusion) {
        ExplosionImpact impact;
        if (p.radius < 1.0e-5f) return impact;

        const double doubleRadius = static_cast<double>(p.radius) * 2.0;
        const glm::dvec3 d = entityPos - p.center;
        const double dist = std::sqrt(glm::dot(d, d)) / doubleRadius;
        if (dist > 1.0) return impact;
        impact.inRange = true;

        // MC uses the entity's EYE position for the push origin; a loose item
        // or an orb has no eyes, and its box is a few pixels tall, so the
        // centre of the box is the same answer.
        const glm::dvec3 boxCentre = (glm::dvec3(box.min) + glm::dvec3(box.max)) * 0.5;
        glm::dvec3 dir = boxCentre - p.center;
        const double len = glm::length(dir);
        // MC Vec3.normalize (world/phys/Vec3.java:67-70):
                //     dist < (double)1.0E-5F ? ZERO : new Vec3(x/dist, ...)
                // The threshold is the FLOAT literal 1.0E-5F widened to double
                // (1.0000000116860974e-05), not 1e-5, and the test is `<` on
                // the zero branch. Ours was 1e-8 with the branches inverted,
                // which left a near-degenerate direction normalised where
                // vanilla zeroes it.
                dir = len < static_cast<double>(1.0e-5f) ? glm::dvec3(0.0) : dir / len;

        const bool wantExposure = p.damageEntities || p.knockbackMultiplier != 0.0f;
        const float exposure =
            wantExposure ? ExplosionSeenPercentBox(level, p.center, box, occlusion) : 0.0f;

        if (p.damageEntities) {
            impact.damage = ExplosionDamageAmount(p.radius, dist, exposure);
        }
        // No EXPLOSION_KNOCKBACK_RESISTANCE term: only a LivingEntity carries
        // attributes, and nothing that reaches this path is one.
        const double power = (1.0 - dist) * static_cast<double>(exposure) *
                             static_cast<double>(p.knockbackMultiplier);
        impact.knockback = dir * power;
        return impact;
    }

    float ExplosionDamageAmount(float radius, double distanceNormalised, float exposure) {
        const double doubleRadius = static_cast<double>(radius) * 2.0;
        const double pw = (1.0 - distanceNormalised) * static_cast<double>(exposure);
        return static_cast<float>((pw * pw + pw) / 2.0 * 7.0 * doubleRadius + 1.0);
    }

    namespace {

        // ── Pass 1: which cells does the blast reach? ──────────────────────
        std::vector<glm::ivec3> CalculateExplodedPositions(
                EntityLevel& level, const ExplosionParams& p, const float* jitter) {
            // The dominant term: 1352 rays, ~17k steps, ~17k chunk lookups for
            // a radius-4 blast. Without a zone here every bit of it is
            // attributed to MobTick.
            PROFILE_ZONE_N("Explosion.RayMarch");
            std::vector<glm::ivec3> out;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return out;

            // Only touched on the inline path. When `jitter` is supplied the
            // draws already happened on the tick thread, which is what makes
            // this function safe to run off it — JavaRandom is not thread-safe,
            // and it is the ONLY shared mutable state the march would reach.
            JavaRandom& rng = level.Random();
            int jitterIndex = 0;

            const int minY = level.GetMinY();
            const int maxY = level.GetMaxY();

            // ── Reach box and the two things it buys ──────────────────────
            //
            // A ray starts with at most radius * 1.3 power and loses at least
            // kPowerPerStep per step, so it can never leave a cube of this
            // half-width around the centre. Two uses:
            //
            //   1. The "reached" set is a dense bitset over that cube rather
            //      than an unordered_set of packed positions. The march
            //      inserts once per step (~20k inserts for a radius-4 blast)
            //      and the hash set was most of the scan's cost. Bounded by a
            //      32 KB cap; a blast bigger than that (radius > ~15, which
            //      only a command can ask for) takes the set.
            //   2. If the whole cube is provably air, no step needs a state
            //      read: every cell is reached and none resists. Same output,
            //      no chunk lookups — and in a mass detonation the pile's
            //      neighbourhood IS all air after the first few blasts.
            const int reach = static_cast<int>(std::ceil(
                                  (p.radius * (kPowerJitterLo + kPowerJitterHi)) /
                                  kPowerPerStep * kStepSize)) + 2;
            const glm::ivec3 c0(static_cast<int>(std::floor(p.center.x)),
                                static_cast<int>(std::floor(p.center.y)),
                                static_cast<int>(std::floor(p.center.z)));
            const glm::ivec3 boxLo = c0 - glm::ivec3(reach);
            const glm::ivec3 boxHi = c0 + glm::ivec3(reach);
            const int side = 2 * reach + 1;
            const bool useBits = static_cast<size_t>(side) * side * side <= (32u * 1024u * 8u);
            std::vector<uint64_t> bits;
            if (useBits) bits.assign((static_cast<size_t>(side) * side * side + 63) / 64, 0ull);
            std::unordered_set<int64_t> reached;
            const bool allAir = blocks->IsRegionAllAir(boxLo, boxHi);

            const auto markReached = [&](const glm::ivec3& cell) {
                if (useBits) {
                    const glm::ivec3 d = cell - boxLo;
                    if (static_cast<unsigned>(d.x) >= static_cast<unsigned>(side) ||
                        static_cast<unsigned>(d.y) >= static_cast<unsigned>(side) ||
                        static_cast<unsigned>(d.z) >= static_cast<unsigned>(side)) {
                        reached.insert(PackPos(cell));   // cannot happen; belt and braces
                        return;
                    }
                    const size_t i = (static_cast<size_t>(d.y) * side + d.z) * side + d.x;
                    bits[i >> 6] |= (1ull << (i & 63));
                } else {
                    reached.insert(PackPos(cell));
                }
            };

            for (int xx = 0; xx < kGridSize; ++xx) {
                for (int yy = 0; yy < kGridSize; ++yy) {
                    for (int zz = 0; zz < kGridSize; ++zz) {
                        // SHELL ONLY — 1352 rays, not 4096. The interior cells
                        // would trace directions already covered by the shell.
                        if (xx != 0 && xx != kGridSize - 1 &&
                            yy != 0 && yy != kGridSize - 1 &&
                            zz != 0 && zz != kGridSize - 1) continue;

                        double dx = static_cast<double>(
                            static_cast<float>(xx) / (kGridSize - 1.0f) * 2.0f - 1.0f);
                        double dy = static_cast<double>(
                            static_cast<float>(yy) / (kGridSize - 1.0f) * 2.0f - 1.0f);
                        double dz = static_cast<double>(
                            static_cast<float>(zz) / (kGridSize - 1.0f) * 2.0f - 1.0f);
                        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (len < 1.0e-9) continue;
                        dx /= len; dy /= len; dz /= len;

                        // Per-ray power jitter. THIS is what makes the crater
                        // ragged; without it a TNT blast is a clean hemisphere
                        // and reads as wrong immediately.
                        // One draw per ray that gets here, and the set of rays
                        // that get here depends only on kGridSize — so the
                        // count is the constant kExplosionRayCount and the
                        // pre-drawn buffer lines up index-for-index.
                        const float roll = jitter ? jitter[jitterIndex++]
                                                  : rng.NextFloat();
                        float power = p.radius *
                            (kPowerJitterLo + roll * kPowerJitterHi);

                        double px = p.center.x, py = p.center.y, pz = p.center.z;

                        // Last-cell memo, reset PER RAY. The march advances
                        // 0.3 of a block per step, so ~60% of steps re-floor to
                        // the cell the previous step already read. The state
                        // read and BlockResistance are pure functions of the
                        // cell, and the march is read-only over its whole
                        // extent (nothing in Explode writes a block until
                        // InteractWithBlocks, which runs after), so a repeat
                        // read is guaranteed to return what the memo holds.
                        //
                        // Per RAY and not per blast: every ray starts at
                        // p.center, so a blast-wide memo buys no extra hits
                        // (measured) while widening the window over which the
                        // "no writer" invariant has to hold from ~0.35 us to
                        // the whole 467 us march.
                        //
                        // haveMemo is an explicit flag, NOT a sentinel cell
                        // value: lastCell(0) would take a false hit on step 1
                        // of any blast whose first step floors to the origin.
                        bool       haveMemo = false;
                        glm::ivec3 lastCell(0);
                        BlockState lastState{};
                        bool       lastHasBlock = false;
                        float      lastResistance = 0.0f;

                        for (; power > 0.0f; power -= kPowerPerStep) {
                            const glm::ivec3 cell(
                                static_cast<int>(std::floor(px)),
                                static_cast<int>(std::floor(py)),
                                static_cast<int>(std::floor(pz)));
                            // MC isInWorldBounds — the DIMENSION's limits, not
                            // the overworld constants.
                            if (cell.y < minY || cell.y > maxY) break;

                            // ONE state read per step, shared by both
                            // predicates. Order is unchanged and must stay so:
                            // BlockResistance runs unconditionally and may
                            // subtract, and only THEN is power re-tested to
                            // gate ShouldBlockExplode and the insert. That test
                            // decides whether the cell the ray died in is part
                            // of the crater.
                            if (allAir) {
                                // Every cell in the box is air: no resistance,
                                // nothing to read. See the reach-box note.
                                lastHasBlock = false;
                                lastResistance = 0.0f;
                                lastState = BlockState{};
                            } else if (!haveMemo || cell != lastCell) {
                                lastState = blocks->GetBlockState(cell.x, cell.y, cell.z);
                                // Re-zeroed on every miss, exactly as the plain
                                // `float resistance = 0.0f;` did: BlockResistance
                                // leaves its out-param untouched when it returns
                                // false (air, spared portal block).
                                lastResistance = 0.0f;
                                lastHasBlock =
                                    BlockResistance(p, *blocks, cell, lastState, lastResistance);
                                lastCell = cell;
                                haveMemo = true;
                            }
                            // Stays GATED on lastHasBlock. Making it
                            // unconditional would cost every air step an extra
                            // (0 + 0.3) * 0.3 on top of kPowerPerStep, cutting
                            // per-ray reach from power/0.225 to power/0.315 —
                            // a ~29% smaller crater on every blast in the game.
                            if (lastHasBlock) {
                                power -= (lastResistance + kResistanceBias) * kResistanceBias;
                            }
                            // NOT memoised: it takes the per-step `power`.
                            if (power > 0.0f &&
                                ShouldBlockExplode(p, *blocks, cell, lastState, power)) {
                                markReached(cell);
                            }

                            px += dx * kStepSize;
                            py += dy * kStepSize;
                            pz += dz * kStepSize;
                        }
                    }
                }
            }

            if (useBits) {
                for (int dy = 0; dy < side; ++dy) {
                    for (int dz = 0; dz < side; ++dz) {
                        const size_t row = (static_cast<size_t>(dy) * side + dz) * side;
                        for (int dx = 0; dx < side; ) {
                            const size_t i = row + dx;
                            const uint64_t w = bits[i >> 6] >> (i & 63);
                            if (w == 0ull) { dx += 64 - static_cast<int>(i & 63); continue; }
                            if (w & 1ull) out.push_back(boxLo + glm::ivec3(dx, dy, dz));
                            ++dx;
                        }
                    }
                }
            }
            out.reserve(out.size() + reached.size());
            for (int64_t k : reached) out.push_back(UnpackPos(k));
            return out;
        }

        // One victim's exposure, computed off the tick thread in phase A of
        // HurtEntities. `valid` distinguishes "computed 0.0" from "not
        // computed" — phase B falls back to computing inline when it is false,
        // which is what makes the split exact rather than approximate.
        struct CachedExposure {
            float exposure = 0.0f;
            bool  valid    = false;
        };

        // ── Per-blast occlusion grid ────────────────────────────────────
        //
        // Exposure is the dominant cost of an explosion by an order of
        // magnitude: every victim in range casts 27 rays at the blast centre,
        // and a thousand victims is a thousand times 27 voxel walks over the
        // SAME few thousand cells. Reading "does this cell collide" once per
        // cell instead of once per visit turns each of those reads into a bit
        // test against an L1-resident bitset.
        //
        // The bit is exactly BlockRegistry::HasCollision(GetBlockState(c)) —
        // the same expression ClipBlocksCollider evaluates first today — so a
        // clear bit skips a cell the walk would have skipped anyway and a set
        // bit falls through to the unchanged shape test. Cells outside the box
        // take the original path, so the box bound is a performance question
        // and never a correctness one.
        struct BlastOcclusion {
            CollisionGrid grid;
            bool          valid = false;
        };

        static int CeilLog2(int n) {
            int s = 0;
            while ((1 << s) < n) ++s;
            return s;
        }

        // Grids are rebuilt per blast; the storage is not. CollisionGrid holds
        // borrowed pointers into these, so a SECOND build on this thread while
        // a grid is still live would dangle them. Nothing today can do that —
        // no Hurt override detonates synchronously — but the guard below makes
        // that a fallback to the plain path rather than a use-after-free if it
        // ever changes.
        thread_local std::vector<uint64_t> t_gridWords;
        thread_local std::vector<uint64_t> t_gridCoarse;
        thread_local bool                  t_gridInUse = false;

        static BlastOcclusion BuildBlastOcclusion(const EntityLevel& level,
                                                  const glm::ivec3& lo,
                                                  const glm::ivec3& hi) {
            PROFILE_ZONE_N("Explosion.BuildOcclusion");
            BlastOcclusion out;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return out;
            if (t_gridInUse) return out;   // see t_gridInUse

            const glm::ivec3 size = hi - lo + glm::ivec3(1);
            if (size.x <= 0 || size.y <= 0 || size.z <= 0) return out;
            CollisionGrid& g = out.grid;
            // Sampled BEFORE the field is read, never after. Stamping it
            // afterwards would record a write that landed DURING the fill as
            // already included: the snapshot would be torn and the guard in
            // SeenPercentOfBox would wave it through.
            g.writeEpoch = level.BlockWriteEpoch();
            g.origin = lo;
            g.size   = size;
            g.shiftZ = CeilLog2(size.x);
            g.shiftY = g.shiftZ + CeilLog2(size.z);

            // Cap on the PADDED word count, which is what actually gets
            // allocated and walked — a raw-volume cap does not bound it. A tall
            // thin box such as 33x240x33 is only 261k cells, under a 64^3 cap,
            // but its power-of-two strides make it 983k bits = 120 KB, far past
            // L1 and never released from the thread_local. Above the cap the
            // build stops paying for itself anyway; fall back to the plain path.
            const size_t bits  = size_t(size.y) << g.shiftY;
            const size_t words = (bits + 63) / 64;
            // 256 KB: a batched cluster's grid is a 64-block cell plus reach
            // (~84^3 = 74 KB); the per-blast grid is far under either cap.
            if (words > (128u * 128u * 128u) / 64u) return out;   // 256 KB
            t_gridWords.assign(words, 0ull);
            blocks->FillCollisionMask(lo, size, g.shiftZ, g.shiftY, t_gridWords.data());
            g.words = t_gridWords.data();

            // Coarse plane, one bit per 4x4x4, for the clear-hull early-out.
            g.coarseSize   = (size + glm::ivec3(3)) / 4;
            g.coarseShiftZ = CeilLog2(g.coarseSize.x);
            g.coarseShiftY = g.coarseShiftZ + CeilLog2(g.coarseSize.z);
            const size_t cbits  = size_t(g.coarseSize.y) << g.coarseShiftY;
            t_gridCoarse.assign((cbits + 63) / 64, 0ull);
            for (int dy = 0; dy < size.y; ++dy) {
                for (int dz = 0; dz < size.z; ++dz) {
                    const size_t row = (size_t(dy) << g.shiftY) | (size_t(dz) << g.shiftZ);
                    for (int dx = 0; dx < size.x; ) {
                        const size_t i = row + size_t(dx);
                        const uint64_t w = t_gridWords[i >> 6];
                        if (w == 0ull) {
                            // Skip to the next word boundary — in a crater most
                            // of the box is empty and this is the common path.
                            dx += 64 - int(i & 63);
                            continue;
                        }
                        if ((w >> (i & 63)) & 1ull) {
                            const size_t ci = (size_t(dy >> 2) << g.coarseShiftY) |
                                              (size_t(dz >> 2) << g.coarseShiftZ) |
                                              size_t(dx >> 2);
                            t_gridCoarse[ci >> 6] |= (1ull << (ci & 63));
                            // The rest of this coarse cell adds nothing.
                            dx += 4 - (dx & 3);
                            continue;
                        }
                        ++dx;
                    }
                }
            }
            g.coarse = t_gridCoarse.data();

            out.valid = true;
            t_gridInUse = true;
            return out;
        }

        // Releases the thread_local storage claim. Scoped so it runs on every
        // exit path out of HurtEntities.
        struct BlastOcclusionScope {
            bool held;
            explicit BlastOcclusionScope(bool h) : held(h) {}
            ~BlastOcclusionScope() { if (held) t_gridInUse = false; }
            BlastOcclusionScope(const BlastOcclusionScope&) = delete;
            BlastOcclusionScope& operator=(const BlastOcclusionScope&) = delete;
        };

        // ── Pass 2: hurt and push everything in range ──────────────────────
        void HurtEntities(EntityLevel& level, const ExplosionParams& p,
                          const CollisionGrid* grid) {
            PROFILE_ZONE_N("Explosion.HurtEntities");
            if (p.radius < 1.0e-5f) return;

            // MC ServerExplosion.hurtEntities:160-168 — verbatim:
            //     float doubleRadius = this.radius * 2.0F;
            //     int x0 = Mth.floor(center.x - (double)doubleRadius - 1.0);
            //     ... (all six bounds floored) ...
            //     new AABB((double)x0, (double)y0, (double)z0, (double)x1, ...)
            //
            // Two divergences fixed at once: doubleRadius is a FLOAT in MC,
            // widened per use, and the six bounds are FLOORED to integers. Ours
            // computed in double and did not floor, so the query box was a
            // sliver larger than vanilla's and could pick up an entity MC
            // misses right at the boundary.
            const float  doubleRadiusF = p.radius * 2.0f;
            const double doubleRadius  = static_cast<double>(doubleRadiusF);
            const double x0 = std::floor(p.center.x - doubleRadius - 1.0);
            const double y0 = std::floor(p.center.y - doubleRadius - 1.0);
            const double z0 = std::floor(p.center.z - doubleRadius - 1.0);
            const double x1 = std::floor(p.center.x + doubleRadius + 1.0);
            const double y1 = std::floor(p.center.y + doubleRadius + 1.0);
            const double z1 = std::floor(p.center.z + doubleRadius + 1.0);
            AABB box;
            box.min = glm::vec3(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z0));
            box.max = glm::vec3(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(z1));

            std::vector<Entity*> nearby;
            level.GetEntitiesInBox(box, p.source, nearby);
            // The quadratic term, made visible: one blast's victim count. At
            // 100k packed TNT this is the whole cost of the pass, because each
            // victim pays an exposure raycast.
            PROFILE_PLOT("Blast/Victims", static_cast<int64_t>(nearby.size()));

            // ── Phase A: exposure, in parallel ──────────────────────────────
            //
            // Exposure is ~87% of a blast and is a PURE function of (block
            // field, blast centre, victim box): it raycasts against blocks and
            // reads nothing else. Nothing in the serial pass below writes a
            // block, so the field is invariant for the whole pass and these can
            // be computed in any order, on any thread.
            //
            // That invariant is held by AUDIT, not by a runtime guard — the
            // write-epoch check in SeenPercentOfBox protects the GRID, and
            // cannot see that a cached exposure was produced before phase B
            // began. What the audit found: no Hurt or Die override in the tree
            // writes a block. The two near misses are Wither::Hurt, which only
            // arms a 20-tick timer consumed in the wither's own later tick, and
            // EnderDragon::CheckWalls, which is reached from the dragon's MOVE
            // path and never from Hurt. Every World::SetBlock caller is on the
            // tick thread. If a Hurt override ever gains a block write, this
            // hoist has to be revisited — the epoch guard will NOT catch it.
            //
            // What is NOT hoisted: every mutation, and every cheap predicate.
            // Phase B re-evaluates IsRemoved / IgnoreExplosion / dist exactly
            // where the old single loop did, so an earlier victim's Hurt still
            // suppresses a later one. Only the expensive pure value is reused,
            // and only when phase B agrees it is wanted. That makes the
            // parallel result identical to the serial one rather than merely
            // equivalent — the order of every Hurt, every AddDeltaMovement and
            // every RNG draw those reach is untouched.
            std::vector<CachedExposure> cached;
            const size_t victimCount = nearby.size();
            // Below this the fork/join costs more than it saves: post-grid
            // exposure is ~1.3 us a victim, and a join is tens of us.
            constexpr size_t kParallelThreshold = 128;
            if (victimCount >= kParallelThreshold && Core::ParallelWidth() > 1) {
                PROFILE_ZONE_N("Explosion.ExposureParallel");
                cached.assign(victimCount, CachedExposure{});
                // ── Exposure memo, keyed on the victim's BLOCK CELL ────────
                //
                // Same sharing as before — victims in one cell take the cell's
                // exposure, computed from the FIRST victim in gather order —
                // but built lock-free: a serial pass lists the unique cells,
                // a parallel pass computes one exposure per cell, and a last
                // serial pass fans the values out. The previous form memoised
                // through a single mutex taken once per victim, and the first
                // blast of a million-TNT pile (a million victims stacked into
                // a few dozen cells) turned that lock into a convoy that
                // stalled the server for over a minute.
                struct UniqueCell { glm::dvec3 pos; size_t firstVictim; float seen; };
                std::unordered_map<int64_t, int> cellOf;
                std::vector<UniqueCell> uniqueCells;
                std::vector<int> victimCell(victimCount, -1);
                for (size_t i = 0; i < victimCount; ++i) {
                    Entity* e = nearby[i];
                    if (!e || e->IsRemoved() || e->IgnoreExplosion()) continue;
                    const double d =
                        std::sqrt(e->DistanceToSqr(p.center.x, p.center.y, p.center.z)) /
                        doubleRadius;
                    if (d > 1.0) continue;
                    const bool damageIt = p.damageEntities &&
                        (!p.calculator.shouldDamageEntity ||
                          p.calculator.shouldDamageEntity(p, *e));
                    if (!(damageIt || p.knockbackMultiplier != 0.0f)) continue;
                    const int64_t cellKey = PackPos(glm::ivec3(
                        static_cast<int>(std::floor(e->position.x)),
                        static_cast<int>(std::floor(e->position.y)),
                        static_cast<int>(std::floor(e->position.z))));
                    const auto it = cellOf.find(cellKey);
                    if (it == cellOf.end()) {
                        cellOf.emplace(cellKey, static_cast<int>(uniqueCells.size()));
                        victimCell[i] = static_cast<int>(uniqueCells.size());
                        uniqueCells.push_back(UniqueCell{e->position, i, 0.0f});
                    } else {
                        victimCell[i] = it->second;
                    }
                }
                Core::ParallelFor(uniqueCells.size(), 1, [&](size_t c) {
                    uniqueCells[c].seen = ExplosionSeenPercent(
                        level, p.center, *nearby[uniqueCells[c].firstVictim], grid);
                });
                for (size_t i = 0; i < victimCount; ++i) {
                    if (victimCell[i] < 0) continue;
                    cached[i].exposure = uniqueCells[static_cast<size_t>(victimCell[i])].seen;
                    cached[i].valid    = true;
                }
            }

            // ── Phase B: apply, serially, in the original order ─────────────
            for (size_t vi = 0; vi < nearby.size(); ++vi) {
                Entity* e = nearby[vi];
                if (!e || e->IsRemoved()) continue;

                // MC entity.ignoreExplosion(this) — a warden mid-dig or
                // mid-emerge is untouched by the whole pass, damage and
                // knockback alike.
                if (e->IgnoreExplosion()) continue;

                const double dist =
                    std::sqrt(e->DistanceToSqr(p.center.x, p.center.y, p.center.z)) /
                    doubleRadius;
                if (dist > 1.0) continue;

                // MC: primed TNT is pushed from its POSITION, everything else
                // from its EYES. That single exception is what makes a stack of
                // TNT scatter outward instead of being shoved into the floor.
                const bool isTnt = e->GetType() == EntityTypeId::Tnt;
                const glm::dvec3 origin = isTnt ? e->position : e->GetEyePosition();
                glm::dvec3 dir = origin - p.center;
                const double len = glm::length(dir);
                // MC Vec3.normalize (world/phys/Vec3.java:67-70):
                //     dist < (double)1.0E-5F ? ZERO : new Vec3(x/dist, ...)
                // The threshold is the FLOAT literal 1.0E-5F widened to double
                // (1.0000000116860974e-05), not 1e-5, and the test is `<` on
                // the zero branch. Ours was 1e-8 with the branches inverted,
                // which left a near-degenerate direction normalised where
                // vanilla zeroes it.
                dir = len < static_cast<double>(1.0e-5f) ? glm::dvec3(0.0) : dir / len;

                // MC ExplosionDamageCalculator.shouldDamageEntity, per victim.
                const bool damageThis = p.damageEntities &&
                    (!p.calculator.shouldDamageEntity ||
                      p.calculator.shouldDamageEntity(p, *e));

                // Skip the (expensive) exposure raycast entirely when neither
                // damage nor knockback would use it — MC's own short-circuit.
                const bool wantExposure =
                    damageThis || p.knockbackMultiplier != 0.0f;
                // Reuse phase A's value when it computed one for this victim;
                // otherwise do it here exactly as before. The fallback is what
                // keeps this exact if a cheap predicate flipped between the
                // phases (a prior Hurt ending a warden's dig, say).
                const float exposure =
                    !wantExposure                      ? 0.0f
                    : (vi < cached.size() && cached[vi].valid) ? cached[vi].exposure
                    : ExplosionSeenPercent(level, p.center, *e, grid);

                // A virtual, not a dynamic_cast. No EntityTypeId test can
                // answer "is this a LivingEntity" — it is a BASE class — but a
                // virtual returning `this` can, and it is exact here because
                // there is exactly one unambiguous LivingEntity subobject in
                // every chain in this hierarchy (the reasoning is written out
                // at Entity::AsLiving). The dynamic_cast this replaces was an
                // out-of-line __dynamic_cast walking the RTTI graph, ~17-22 ns
                // against ~0.7 ns, on a call site a profile caught running
                // 10.4 million times.
                auto* living = e->AsLiving();

                if (damageThis) {
                    // Everything GetEntitiesInBox can return IS a LivingEntity
                    // in this engine — mobs and the player views, and the
                    // Mob-shaped entities (primed TNT, falling blocks,
                    // projectiles) documented in Projectile.hpp. Hurt lives on
                    // LivingEntity, so the cast is the call, not a filter.
                    //
                    // The entities MC also hurts that this sweep cannot see —
                    // dropped items and XP orbs, which live in their own
                    // managers outside the Game::Entity hierarchy — are
                    // reached through EntityLevel::ApplyExplosionToLooseEntities
                    // below.
                    if (living) {
                        living->Hurt(MobDamageSource::Explosion,
                                     ExplosionDamageAmount(p.radius, dist, exposure),
                                     p.attributedTo ? p.attributedTo : p.source);
                    }
                }

                // Knockback reaches EVERYTHING, not just the living — dropped
                // items and primed TNT are thrown too, which is most of what a
                // blast looks like.
                //
                // MC scales it by EXPLOSION_KNOCKBACK_RESISTANCE, which only a
                // living entity carries (a wither and the wardens have it, and
                // it is what stops a boss being punted across the arena).
                //
                // Split so the attribute read can be skipped when it provably
                // cannot change the outcome. Nothing in this codebase ever
                // REGISTERS ExplosionKnockbackResistance, so GetAttributeValue
                // is a guaranteed-miss lookup on every victim of every blast.
                // Exact: when the leading product is zero the final power is
                // +/-0.0 for any finite resistance and NaN for a non-finite
                // one, and `power > 0.0` is false for +0.0, -0.0 and NaN alike,
                // so the branch below goes the same way whatever the attribute
                // holds. GetAttributeValue is const and does not lazily insert,
                // so skipping it mutates nothing observable.
                const double preResistance = (1.0 - dist) *
                                             static_cast<double>(exposure) *
                                             static_cast<double>(p.knockbackMultiplier);
                double knockbackResistance = 0.0;
                if (living && preResistance != 0.0) {
                    knockbackResistance = living->GetAttributeValue(
                        Attribute::ExplosionKnockbackResistance);
                }
                const double power = preResistance * (1.0 - knockbackResistance);
                if (power > 0.0) {
                    // MC ServerExplosion.hurtEntities pushes every entity, but
                    // only records a PLAYER's push in `hitPlayers` when they
                    // are neither spectating nor flying in creative — and it is
                    // that recorded value the client is told to apply.
                    //
                    // Here AddDeltaMovement on a PlayerEntityView IS the
                    // recording (it parks the vector for BroadcastExplosion to
                    // drain), so the gate belongs on the call itself. Without
                    // it a spectator watching a TNT chain was thrown across
                    // the world by blasts they are not supposed to feel.
                    const bool excludedPlayer = e->IsPlayer() &&
                        (e->IsSpectator() || (e->IsCreative() && e->IsAbilityFlying()));
                    if (!excludedPlayer) e->AddDeltaMovement(dir * power);
                }
            }
        }

        // ── Pass 3: break the blocks and pop what they drop ────────────────
        void InteractWithBlocks(EntityLevel& level, const ExplosionParams& p,
                                ExplosionBlockInteraction bi,
                                std::vector<glm::ivec3>& toBlow) {
            PROFILE_ZONE_N("Explosion.InteractWithBlocks");
            ILevelWrite* write = level.MutableBlocks();
            if (!write) return;

            JavaRandom& rng = level.Random();

            // MC Util.shuffle. Observable: it decides which of two overlapping
            // drops merges into which stack, and therefore where the merged
            // item entity ends up.
            for (size_t i = toBlow.size(); i > 1; --i) {
                const int j = rng.NextInt(static_cast<int>(i));
                std::swap(toBlow[i - 1], toBlow[static_cast<size_t>(j)]);
            }

            // MC BlockBehaviour.onExplosionHit returns immediately for a
            // TRIGGER_BLOCK blast: a wind charge presses buttons (through the
            // per-block overrides) but breaks and drops nothing. The pass still
            // RUNS for it — that is how those overrides are reached at all —
            // so the guard is here rather than at the call site.
            const bool destroys = (bi != ExplosionBlockInteraction::TriggerBlock);

            // MC ServerExplosion.canTriggerBlocks: a TRIGGER blast triggers,
            // EXCEPT a BREEZE wind charge, which is additionally gated on
            // mobGriefing — a breeze must not open your doors in a world where
            // mobs cannot change blocks. A player-thrown wind charge is not
            // gated.
            const bool canTrigger = !destroys &&
                (p.source == nullptr ||
                 p.source->GetType() != EntityTypeId::BreezeWindCharge ||
                 level.MobGriefing());

            // MC StackCollector: drops merge into stacks BEFORE being popped,
            // so a cobblestone field blasted by TNT spawns a handful of item
            // entities instead of eighty.
            struct Collected { glm::ivec3 pos; ItemStack stack; };
            std::vector<Collected> collected;

            const bool decay = (bi == ExplosionBlockInteraction::DestroyWithDecay);

            // MC BlockBehaviour.onExplosionHit's `doDropExperienceHack`:
            // getIndirectSourceEntity() instanceof Player. Any player-lit TNT
            // qualifies, and it is what makes a blasted ore still pay out.
            Entity* indirect = p.attributedTo;
            const bool dropExperience = indirect && indirect->IsPlayer();
            const int32_t creditId = dropExperience ? indirect->GetId() : -1;

            for (const glm::ivec3& pos : toBlow) {
                const BlockState state = write->GetBlockState(pos.x, pos.y, pos.z);
                const BlockID    id    = state.Block();
                if (id == BlockID::Air) continue;
                if (!destroys) {
                    // MC still dispatches onExplosionHit so a button, lever,
                    // door, gate or candle can respond; nothing here breaks.
                    if (canTrigger) ExplosionTriggerBlock(*write, pos, state);
                    continue;
                }

                if (BlockDropsFromExplosion(id)) {
                    // MC seeds nothing: the loot RNG is the LEVEL's shared
                    // stream, so two identical blocks in one blast roll
                    // differently and the same block blasted twice can differ.
                    //
                    // A pure position hash — which is what this was — made
                    // every roll a deterministic function of coordinates, so
                    // with DESTROY_WITH_DECAY a given block always made the
                    // same survive/decay decision no matter how many times you
                    // rebuilt and reblasted it. Mixing the level stream in
                    // keeps the per-position independence that stops two
                    // blocks in one tick drawing correlated rolls, while
                    // restoring the run-to-run variation vanilla has.
                    JavaRandom lootRng(static_cast<uint64_t>(
                        (static_cast<int64_t>(pos.x) * 3129871) ^
                        (static_cast<int64_t>(pos.z) * 116129781) ^
                         static_cast<int64_t>(pos.y) ^
                        (static_cast<int64_t>(rng.NextInt(1 << 24)) << 24)));
                    LootContext ctx;
                    ctx.block      = id;
                    ctx.blockState = state.Index();
                    ctx.pos        = pos;
                    ctx.blocks     = write;
                    ctx.rng        = &lootRng;
                    // MC passes THIS_ENTITY = explosion.getDirectSourceEntity()
                    // — the TNT, creeper or fireball itself, which is non-null
                    // for every blast this engine can produce. Hardcoding
                    // false made loot conditions that merely test for an entity
                    // take the wrong branch.
                    ctx.brokenByEntity = (p.source != nullptr);
                    // EXPLOSION_RADIUS is supplied ONLY for decay, which is the
                    // switch that turns survives_explosion and
                    // apply_explosion_decay from no-ops into 1/radius rolls.
                    ctx.explosionRadius = decay ? p.radius : -1.0f;

                    for (const ItemStack& drop : LootTables::GetDrops(ctx)) {
                        if (drop.IsEmpty()) continue;
                        // MC ItemEntity.merge(to, from, 16): the cap is
                        // min(item.maxStackSize, 16) and the transfer is
                        // PARTIAL — as much as fits moves, the rest starts a
                        // new stack. The old all-or-nothing test against a
                        // literal 16 both overfilled items whose real cap is
                        // smaller (eggs and snowballs stack to 16, ender
                        // pearls to 16, buckets to 1) and refused merges that
                        // should have partly succeeded.
                        ItemStack remaining = drop;
                        for (Collected& c : collected) {
                            if (remaining.IsEmpty()) break;
                            if (c.stack.itemId != remaining.itemId) continue;
                            const int cap = std::min(
                                ItemRegistry::Get(c.stack.itemId).maxStackSize,
                                kMaxDropsPerStack);
                            const int room = cap - c.stack.count;
                            if (room <= 0) continue;
                            const int moved = std::min(room, remaining.count);
                            c.stack.count   += moved;
                            remaining.count -= moved;
                        }
                        if (!remaining.IsEmpty()) {
                            collected.push_back(Collected{pos, remaining});
                        }
                    }
                }

                // MC BlockBehaviour.onExplosionHit calls state.spawnAfterBreak
                // with the experience hack flag. That is what pays out a
                // blasted ore, sculk block or spawner — nothing did before, so
                // TNT-mining an ore silently ate its XP.
                if (dropExperience) {
                    JavaRandom xpRng(static_cast<uint64_t>(
                        (static_cast<int64_t>(pos.x) * 2654435761LL) ^
                        (static_cast<int64_t>(pos.y) * 40503LL) ^
                        (static_cast<int64_t>(pos.z) * 2246822519LL) ^
                         static_cast<int64_t>(rng.NextInt(1 << 24))));
                    const int xp = LootTables::RollBlockBreakExperience(id, nullptr, xpRng);
                    if (xp > 0) {
                        level.AwardExperience(
                            glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                            xp, creditId);
                    }
                }

                // MC setBlock(pos, AIR, 3) — neighbours AND clients. The
                // neighbour half is what makes the sand above a crater notice
                // it has lost its floor.
                write->SetBlock(pos.x, pos.y, pos.z, BlockID::Air,
                                World::UpdateFlags::All);

                // MC block.wasExploded — TNT's is what chain-detonates. Called
                // AFTER the cell is cleared, exactly as vanilla does, so the
                // primed entity does not spawn inside the block it replaces.
                if (id == BlockID::Tnt) {
                    level.OnTntExploded(pos, p.attributedTo);
                }
            }

            for (const Collected& c : collected) {
                DropItemStackNear(level.Dimension(), c.pos, c.stack);
            }
        }

        // MC ServerExplosion.createFire — one in three eligible cells.
        void CreateFire(EntityLevel& level, const std::vector<glm::ivec3>& toBlow) {
            ILevelWrite* write = level.MutableBlocks();
            const IBlockAccess* blocks = level.Blocks();
            if (!write || !blocks) return;
            JavaRandom& rng = level.Random();

            for (const glm::ivec3& pos : toBlow) {
                if (rng.NextInt(3) != 0) continue;
                if (blocks->GetBlock(pos.x, pos.y, pos.z) != BlockID::Air) continue;
                // MC isSolidRender on the block below — fire needs a floor.
                //
                // isSolidRender is per-STATE and means `isShapeFullBlock(
                // occlusionShape)`: the block's outline must fill the whole
                // cell. The per-BLOCK `opaque` flag this used to read is a
                // render-layer classification, and it says yes for slabs,
                // stairs and every other partial opaque block — so a blast
                // used to light fires standing on top of a bottom slab, where
                // vanilla lights none.
                const BlockState belowState =
                    blocks->GetBlockState(pos.x, pos.y - 1, pos.z);
                if (belowState.Block() == BlockID::Air) continue;
                if (!BlockRegistry::GetBlockShapeSet(belowState).IsFullCube()) continue;
                write->SetBlock(pos.x, pos.y, pos.z, BlockID::Fire,
                                World::UpdateFlags::All);
            }
        }

    } // namespace

    // The no-batcher default. Out of line so EntityLevel.hpp does not have to
    // pull in this header just to name Explode().
    void EntityLevel::QueueExplosion(const ExplosionParams& p) {
        Explode(*this, p);
    }

    std::vector<glm::ivec3> ExplosionScanCrater(EntityLevel& level,
                                                const ExplosionParams& p,
                                                const float* jitter) {
        if (level.IsClientSide() || p.radius <= 0.0f) return {};
        return CalculateExplodedPositions(level, p, jitter);
    }

    ExplosionResult Explode(EntityLevel& level, const ExplosionParams& p) {
        PROFILE_ZONE_N("Explode");
        // MC ClientLevel.explode is an empty method: the client is TOLD about
        // a blast, it never simulates one. Running it here would destroy blocks
        // the server has not agreed to and then have to rewind them.
        if (level.IsClientSide()) return {};
        if (p.radius <= 0.0f) return {};

        // Inline jitter: this is the ONE-blast path, already on the thread that
        // owns the RNG. The batched path draws up front instead — see
        // ExplosionScanCrater.
        std::vector<glm::ivec3> toBlow = CalculateExplodedPositions(level, p, nullptr);
        return ExplodeApply(level, p, toBlow);
    }

    namespace {
        ExplosionResult ExplodeApplyBlocks(EntityLevel& level, const ExplosionParams& p,
                                           std::vector<glm::ivec3>& toBlow);
    }

    ExplosionResult ExplodeApply(EntityLevel& level, const ExplosionParams& p,
                                 std::vector<glm::ivec3>& toBlow) {
        PROFILE_ZONE_N("Explode.Apply");
        ExplosionResult result;
        if (level.IsClientSide()) return result;
        if (p.radius <= 0.0f) return result;

        // One collision snapshot for the whole victim pass — the entity sweep
        // and the loose-entity sweep both read the same neighbourhood, so they
        // share it. Built AFTER the ray march (which is read-only) and read
        // ONLY by the two victim passes below — nothing consults it after
        // InteractWithBlocks, which is the first thing in a blast that writes a
        // block. The epoch guard in SeenPercentOfBox is the backstop if that
        // ever stops being true.
        //
        // Sized geometrically rather than from the victims: everything hurt is
        // within 2*radius of the centre, and the box bound is a PERFORMANCE
        // question, not a correctness one — a cell outside it simply takes the
        // uncached path, which is what every cell took before this existed. So
        // a large victim whose box pokes out of it is handled correctly, it
        // just gets less acceleration.
        BlastOcclusion occl;
        {
            const int reach = static_cast<int>(std::ceil(
                                  static_cast<double>(p.radius) * 2.0)) + 2;
            const double kLim = 3.0e7;
            if (reach > 0 && std::isfinite(p.center.x) && std::isfinite(p.center.y) &&
                std::isfinite(p.center.z) &&
                std::abs(p.center.x) < kLim && std::abs(p.center.y) < kLim &&
                std::abs(p.center.z) < kLim) {
                const glm::ivec3 c(static_cast<int>(std::floor(p.center.x)),
                                   static_cast<int>(std::floor(p.center.y)),
                                   static_cast<int>(std::floor(p.center.z)));
                occl = BuildBlastOcclusion(level, c - glm::ivec3(reach),
                                                  c + glm::ivec3(reach));
            }
        }
        const BlastOcclusionScope occlScope(occl.valid);
        const CollisionGrid* grid = occl.valid ? &occl.grid : nullptr;

        HurtEntities(level, p, grid);
        // The victims the entity sweep cannot see: dropped items and XP orbs
        // live in their own managers outside Game::Entity.
        level.ApplyExplosionToLooseEntities(p, grid);

        return ExplodeApplyBlocks(level, p, toBlow);
    }

    namespace {
        // The mutating tail of a blast after its victims: the block breaking,
        // the fire, the sound and the broadcast. Shared by the per-blast and
        // the batched apply so the two cannot drift.
        ExplosionResult ExplodeApplyBlocks(EntityLevel& level, const ExplosionParams& p,
                                           std::vector<glm::ivec3>& toBlow) {
            ExplosionResult result;
            const ExplosionBlockInteraction bi = ResolveInteraction(level, p.interaction);

            // MC ServerExplosion.explode returns toBlow.size() unconditionally —
            // the count is "how many cells the blast REACHED", which is what the
            // client's debris loop wants, not "how many it broke". Assigning it
            // only inside the interaction branch made a creeper with mobGriefing
            // off report zero and draw no debris at all.
            result.blocksDestroyed = static_cast<int>(toBlow.size());

            if (InteractsWithBlocks(bi)) {
                InteractWithBlocks(level, p, bi, toBlow);
            }
            if (p.fire) CreateFire(level, toBlow);

            if (p.spawnVisual) {
                // MC's client plays this at volume 4.0 with pitch
                // (1 + (rand - rand) * 0.2) * 0.7.
                PlaySound("entity.generic.explode", p.center, 4.0f, 0.7f);
                // MC Explosion.isSmall(): under radius 2, or nothing was touched.
                const bool small = p.radius < kLargeExplosionRadius ||
                                   !InteractsWithBlocks(bi);
                level.BroadcastExplosion(p.center, p.radius, result.blocksDestroyed, small);
            }
            return result;
        }

        // MC ServerExplosion.hurtEntities' query box, floored exactly as
        // vanilla does (see HurtEntities for the float/floor notes).
        AABB HurtBoxOf(const ExplosionParams& p) {
            const double doubleRadius = static_cast<double>(p.radius * 2.0f);
            AABB box;
            box.min = glm::vec3(static_cast<float>(std::floor(p.center.x - doubleRadius - 1.0)),
                                static_cast<float>(std::floor(p.center.y - doubleRadius - 1.0)),
                                static_cast<float>(std::floor(p.center.z - doubleRadius - 1.0)));
            box.max = glm::vec3(static_cast<float>(std::floor(p.center.x + doubleRadius + 1.0)),
                                static_cast<float>(std::floor(p.center.y + doubleRadius + 1.0)),
                                static_cast<float>(std::floor(p.center.z + doubleRadius + 1.0)));
            return box;
        }
    } // namespace

    int64_t ExplodeApplyBatch(EntityLevel& level, std::vector<ExplosionParams>& params,
                              std::vector<std::vector<glm::ivec3>>& craters,
                              int64_t* outBlastNs) {
        PROFILE_ZONE_N("Explode.ApplyBatch");
        if (level.IsClientSide()) return 0;
        const size_t count = params.size();
        if (count == 0) return 0;
        const auto batchStart = std::chrono::steady_clock::now();

        // ── Clusters: 64-block cells, first-seen order ──────────────────────
        // A cluster exists to bound ONE occlusion grid and to scope the
        // serial damage pass; every victim-scaled structure below is built
        // once per tick, not per cluster — the per-cluster rebuild was most
        // of the apply at a million victims.
        struct Cluster { std::vector<size_t> blasts; };
        std::vector<Cluster> clusters;
        {
            std::unordered_map<int64_t, size_t> byCell;
            for (size_t i = 0; i < count; ++i) {
                const ExplosionParams& p = params[i];
                if (p.radius <= 0.0f) continue;
                const int64_t key = PackPos(glm::ivec3(
                    static_cast<int>(std::floor(p.center.x)) >> 6,
                    static_cast<int>(std::floor(p.center.y)) >> 6,
                    static_cast<int>(std::floor(p.center.z)) >> 6));
                const auto it = byCell.find(key);
                if (it == byCell.end()) {
                    byCell.emplace(key, clusters.size());
                    clusters.push_back(Cluster{});
                }
                clusters[it == byCell.end() ? clusters.size() - 1 : it->second]
                    .blasts.push_back(i);
            }
        }
        if (clusters.empty()) return 0;

        // ── One gather for the whole tick ───────────────────────────────────
        std::vector<Entity*> everyone;
        {
            PROFILE_ZONE_N("Explosion.GatherBin");
            AABB all = HurtBoxOf(params[clusters[0].blasts[0]]);
            for (const Cluster& cl : clusters) {
                for (size_t bi : cl.blasts) {
                    const AABB hb = HurtBoxOf(params[bi]);
                    all.min = glm::min(all.min, hb.min);
                    all.max = glm::max(all.max, hb.max);
                }
            }
            level.GetEntitiesInBox(all, nullptr, everyone);
        }
        PROFILE_PLOT("Blast/Victims", static_cast<int64_t>(everyone.size()));

        // ── One victim table for the whole tick ─────────────────────────────
        struct Victim {
            Entity*       e        = nullptr;
            LivingEntity* living   = nullptr;
            glm::dvec3    pos{0.0};
            glm::dvec3    eye{0.0};
            int           cell     = -1;     // index into cells (pushOnly only)
            bool          valid    = false;
            bool          pushOnly = false;
            bool          isTnt    = false;
        };
        struct Cell { glm::dvec3 pos; AABBd box; };
        const size_t N = everyone.size();
        std::vector<Victim>  victims(N);
        std::vector<int64_t> cellKeys(N, 0);
        std::vector<Cell>    cells;
        std::unordered_map<int64_t, int> cellIndex;
        std::vector<size_t>  damageable;   // victims the serial damage pass visits
        {
            PROFILE_ZONE_N("Explosion.VictimTable");
            const auto fill = [&](size_t i) {
                Entity* e = everyone[i];
                Victim& v = victims[i];
                if (!e || e->IsRemoved() || e->IgnoreExplosion()) return;
                v.e        = e;
                v.living   = e->AsLiving();
                v.pos      = e->position;
                v.eye      = e->GetEyePosition();
                v.isTnt    = e->GetType() == EntityTypeId::Tnt;
                v.pushOnly = e->ExplosionPushOnly() && !e->IsPlayer();
                v.valid    = true;
                if (v.pushOnly) {
                    cellKeys[i] = PackPos(glm::ivec3(
                        static_cast<int>(std::floor(v.pos.x)),
                        static_cast<int>(std::floor(v.pos.y)),
                        static_cast<int>(std::floor(v.pos.z))));
                }
            };
            if (N >= 512 && Core::ParallelWidth() > 1) {
                Core::ParallelFor(N, 256, fill);
            } else {
                for (size_t i = 0; i < N; ++i) fill(i);
            }
            // Cell indices in gather order — the FIRST victim in a cell
            // represents it, as the per-blast memo always did.
            for (size_t i = 0; i < N; ++i) {
                Victim& v = victims[i];
                if (!v.valid) continue;
                if (!v.pushOnly) { damageable.push_back(i); continue; }
                const auto it = cellIndex.find(cellKeys[i]);
                if (it == cellIndex.end()) {
                    cellIndex.emplace(cellKeys[i], static_cast<int>(cells.size()));
                    v.cell = static_cast<int>(cells.size());
                    cells.push_back(Cell{v.pos, v.e->GetAABBd()});
                } else {
                    v.cell = it->second;
                }
            }
        }
        const size_t C = cells.size();

        // Cells binned by 16-block tile, so a group finds the cells it can
        // reach by looking at its neighbouring bins instead of scanning all
        // of them.
        std::unordered_map<int64_t, std::vector<int>> cellBins;
        for (size_t c = 0; c < C; ++c) {
            const int64_t key = PackPos(glm::ivec3(
                static_cast<int>(std::floor(cells[c].pos.x)) >> 4,
                static_cast<int>(std::floor(cells[c].pos.y)) >> 4,
                static_cast<int>(std::floor(cells[c].pos.z)) >> 4));
            cellBins[key].push_back(static_cast<int>(c));
        }

        // ── Push groups, global ─────────────────────────────────────────────
        // Blasts sharing (1-block centre cell, radius, knockback multiplier)
        // collapse into one impulse applied `count` times from the first
        // member's centre — see the divergence note on ExplodeApplyBatch's
        // declaration. Below the gate every blast is its own group and the
        // arithmetic matches the ungrouped loop exactly.
        struct PushGroup {
            glm::dvec3 center{0.0};
            float      radius = 0.0f;
            float      kbMult = 1.0f;
            double     cnt    = 0.0;
            size_t     cluster = 0;
        };
        constexpr size_t kPushMergeMinBlasts = 32;
        std::vector<PushGroup> groups;
        std::vector<std::vector<size_t>> clusterGroups(clusters.size());
        {
            std::map<std::tuple<int64_t, int32_t, int32_t>, size_t> groupIndex;
            for (size_t ci = 0; ci < clusters.size(); ++ci) {
                for (size_t bi : clusters[ci].blasts) {
                    const ExplosionParams& p = params[bi];
                    if (count >= kPushMergeMinBlasts) {
                        const auto key = std::make_tuple(
                            PackPos(glm::ivec3(static_cast<int>(std::floor(p.center.x)),
                                               static_cast<int>(std::floor(p.center.y)),
                                               static_cast<int>(std::floor(p.center.z)))),
                            static_cast<int32_t>(p.radius * 256.0f),
                            static_cast<int32_t>(p.knockbackMultiplier * 256.0f));
                        const auto it = groupIndex.find(key);
                        if (it != groupIndex.end()) {
                            groups[it->second].cnt += 1.0;
                            continue;
                        }
                        groupIndex.emplace(key, groups.size());
                    }
                    clusterGroups[ci].push_back(groups.size());
                    groups.push_back(PushGroup{p.center, p.radius,
                                               p.knockbackMultiplier, 1.0, ci});
                }
            }
        }

        // ── Exposure, per cluster (its occlusion grid), appended per cell ──
        // perCellGroups[c] lists exactly the groups whose blast can reach
        // cell c, with the exposure already computed — so the push pass below
        // does work proportional to REAL interactions, not victims x groups.
        std::vector<std::vector<std::pair<int, float>>> perCellGroups(C);
        std::vector<std::vector<std::pair<int, float>>> groupCells(groups.size());
        for (size_t ci = 0; ci < clusters.size(); ++ci) {
            if (clusterGroups[ci].empty()) continue;
            // Grid over this cluster's blast boxes.
            AABB unionBox = HurtBoxOf(params[clusters[ci].blasts[0]]);
            for (size_t bi : clusters[ci].blasts) {
                const AABB hb = HurtBoxOf(params[bi]);
                unionBox.min = glm::min(unionBox.min, hb.min);
                unionBox.max = glm::max(unionBox.max, hb.max);
            }
            BlastOcclusion occl;
            const double kLim = 3.0e7;
            if (std::abs(unionBox.min.x) < kLim && std::abs(unionBox.max.x) < kLim &&
                std::abs(unionBox.min.y) < kLim && std::abs(unionBox.max.y) < kLim &&
                std::abs(unionBox.min.z) < kLim && std::abs(unionBox.max.z) < kLim) {
                occl = BuildBlastOcclusion(level,
                    glm::ivec3(static_cast<int>(std::floor(unionBox.min.x)),
                               static_cast<int>(std::floor(unionBox.min.y)),
                               static_cast<int>(std::floor(unionBox.min.z))),
                    glm::ivec3(static_cast<int>(std::floor(unionBox.max.x)),
                               static_cast<int>(std::floor(unionBox.max.y)),
                               static_cast<int>(std::floor(unionBox.max.z))));
            }
            const BlastOcclusionScope occlScope(occl.valid);
            const CollisionGrid* grid = occl.valid ? &occl.grid : nullptr;

            {
                PROFILE_ZONE_N("Explosion.ExposureParallel");
                const auto& gset = clusterGroups[ci];
                const auto one = [&](size_t k) {
                    const size_t g = gset[k];
                    const PushGroup& grp = groups[g];
                    if (grp.kbMult == 0.0f && true) {
                        // Knockback-only consumers; a damage-only blast still
                        // wants no per-cell exposure (damage path is exact).
                    }
                    const double doubleRadius = static_cast<double>(grp.radius) * 2.0;
                    const int reach = static_cast<int>(std::ceil(doubleRadius)) + 1;
                    const int bx0 = (static_cast<int>(std::floor(grp.center.x)) - reach) >> 4;
                    const int bx1 = (static_cast<int>(std::floor(grp.center.x)) + reach) >> 4;
                    const int by0 = (static_cast<int>(std::floor(grp.center.y)) - reach) >> 4;
                    const int by1 = (static_cast<int>(std::floor(grp.center.y)) + reach) >> 4;
                    const int bz0 = (static_cast<int>(std::floor(grp.center.z)) - reach) >> 4;
                    const int bz1 = (static_cast<int>(std::floor(grp.center.z)) + reach) >> 4;
                    auto& outCells = groupCells[g];
                    for (int bx = bx0; bx <= bx1; ++bx)
                        for (int by = by0; by <= by1; ++by)
                            for (int bz = bz0; bz <= bz1; ++bz) {
                                const auto it = cellBins.find(
                                    PackPos(glm::ivec3(bx, by, bz)));
                                if (it == cellBins.end()) continue;
                                for (const int c : it->second) {
                                    const glm::dvec3 d = cells[c].pos - grp.center;
                                    if (std::sqrt(glm::dot(d, d)) / doubleRadius > 1.0)
                                        continue;
                                    outCells.emplace_back(c,
                                        ExplosionSeenPercentBox(level, grp.center,
                                                                cells[c].box, grid));
                                }
                            }
                };
                if (gset.size() >= 4 && Core::ParallelWidth() > 1) {
                    Core::ParallelFor(gset.size(), 1, one);
                } else {
                    for (size_t k = 0; k < gset.size(); ++k) one(k);
                }
            }

            // ── Damage path for THIS cluster's blasts, under its grid ──────
            // Serial and in order, with exact per-victim exposure; the
            // damageable list (players, real mobs) is tiny next to the pile.
            // Runs here so the exposure raycasts get the cluster's occlusion
            // grid — computing them gridless after the grids were released
            // was measured at over a hundred milliseconds a tick.
            {
                PROFILE_ZONE_N("Explosion.HurtEntities");
                for (size_t bi : clusters[ci].blasts) {
                    const ExplosionParams& p = params[bi];
                    const double doubleRadius = static_cast<double>(p.radius * 2.0f);
                    for (const size_t vi : damageable) {
                        const Victim& v = victims[vi];
                        Entity* e = v.e;
                        if (e == p.source || e->IsRemoved()) continue;
                        if (e->IgnoreExplosion()) continue;
                        const double dist = std::sqrt(e->DistanceToSqr(
                            p.center.x, p.center.y, p.center.z)) / doubleRadius;
                        if (dist > 1.0) continue;

                        const bool isTnt = e->GetType() == EntityTypeId::Tnt;
                        const glm::dvec3 origin = isTnt ? e->position : e->GetEyePosition();
                        glm::dvec3 dir = origin - p.center;
                        const double len = glm::length(dir);
                        dir = len < static_cast<double>(1.0e-5f) ? glm::dvec3(0.0) : dir / len;

                        const bool damageThis = p.damageEntities &&
                            (!p.calculator.shouldDamageEntity ||
                              p.calculator.shouldDamageEntity(p, *e));
                        const bool wantExposure = damageThis || p.knockbackMultiplier != 0.0f;
                        const float expo = wantExposure
                            ? ExplosionSeenPercent(level, p.center, *e, grid) : 0.0f;

                        LivingEntity* living = e->AsLiving();
                        if (damageThis && living) {
                            living->Hurt(MobDamageSource::Explosion,
                                         ExplosionDamageAmount(p.radius, dist, expo),
                                         p.attributedTo ? p.attributedTo : p.source);
                        }
                        const double preResistance = (1.0 - dist) *
                                                     static_cast<double>(expo) *
                                                     static_cast<double>(p.knockbackMultiplier);
                        double knockbackResistance = 0.0;
                        if (living && preResistance != 0.0) {
                            knockbackResistance = living->GetAttributeValue(
                                Attribute::ExplosionKnockbackResistance);
                        }
                        const double power = preResistance * (1.0 - knockbackResistance);
                        if (power > 0.0) {
                            const bool excludedPlayer = e->IsPlayer() &&
                                (e->IsSpectator() || (e->IsCreative() && e->IsAbilityFlying()));
                            if (!excludedPlayer) e->AddDeltaMovement(dir * power);
                        }
                    }
                }
            }
        }
        // Merge in group order, so a victim sums its impulses in the same
        // deterministic order however the exposure threads interleaved.
        for (size_t g = 0; g < groups.size(); ++g) {
            for (const auto& [c, expo] : groupCells[g]) {
                perCellGroups[static_cast<size_t>(c)].emplace_back(
                    static_cast<int>(g), expo);
            }
        }

        // ── Push-only victims: all groups in range, in parallel ─────────────
        std::vector<glm::dvec3> pushes(N, glm::dvec3(0.0));
        {
            PROFILE_ZONE_N("Explosion.PushOnly");
            const auto one = [&](size_t i) {
                Victim& v = victims[i];
                if (!v.valid || !v.pushOnly || v.cell < 0) return;
                glm::dvec3 sum(0.0);
                for (const auto& [g, expo] : perCellGroups[static_cast<size_t>(v.cell)]) {
                    const PushGroup& grp = groups[static_cast<size_t>(g)];
                    if (grp.kbMult == 0.0f) continue;
                    const double doubleRadius = static_cast<double>(grp.radius) * 2.0;
                    const double dist = std::sqrt(v.e->DistanceToSqr(
                        grp.center.x, grp.center.y, grp.center.z)) / doubleRadius;
                    if (dist > 1.0) continue;
                    const glm::dvec3 origin = v.isTnt ? v.pos : v.eye;
                    glm::dvec3 dir = origin - grp.center;
                    const double len = glm::length(dir);
                    dir = len < static_cast<double>(1.0e-5f) ? glm::dvec3(0.0) : dir / len;
                    const double power = (1.0 - dist) * static_cast<double>(expo) *
                                         static_cast<double>(grp.kbMult) * grp.cnt;
                    if (power > 0.0) sum += dir * power;
                }
                pushes[i] = sum;
            };
            if (N >= 512 && Core::ParallelWidth() > 1) {
                Core::ParallelFor(N, 128, one);
            } else {
                for (size_t i = 0; i < N; ++i) one(i);
            }
            for (size_t i = 0; i < N; ++i) {
                const Victim& v = victims[i];
                if (!v.valid || !v.pushOnly) continue;
                if (pushes[i] != glm::dvec3(0.0)) v.e->AddDeltaMovement(pushes[i]);
            }
        }

        // Dropped items and XP orbs: one walk for the whole tick.
        {
            std::vector<const ExplosionParams*> all;
            all.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                if (params[i].radius > 0.0f) all.push_back(&params[i]);
            }
            level.ApplyExplosionsToLooseEntities(all.data(), all.size(), nullptr);
        }

        // Everything above scales with the pile, not the blast count.
        const int64_t fixedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - batchStart).count();

        // ── Blocks, fire, sound, broadcast: serial, queue order ─────────────
        {
            PROFILE_ZONE_N("Explode.Apply");
            const auto blocksStart = std::chrono::steady_clock::now();
            for (size_t i = 0; i < count; ++i) {
                if (params[i].radius <= 0.0f) continue;
                ExplodeApplyBlocks(level, params[i], craters[i]);
            }
            if (outBlastNs) {
                *outBlastNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - blocksStart).count();
            }
        }
        return fixedNs;
    }

} // namespace Game
