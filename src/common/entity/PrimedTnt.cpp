// File: src/common/entity/PrimedTnt.cpp
#include "common/entity/PrimedTnt.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/Explosion.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    PrimedTnt::PrimedTnt(EntityLevel* level)
        : Mob(EntityTypeId::Tnt, level, NoAiTag{}) {
        m_blockState = BlockStates::Default(BlockID::Tnt);

        // Opt out of the dying-mob reference sweep. This type registers no
        // goals, has no brain, never calls SetTarget and has no
        // ClearReferenceTo override, so that pass is a provable no-op for it —
        // and a mass detonation puts a million of these in the level, where the
        // pass is O(dying x surviving). Any real acquisition (being hurt,
        // riding, being ridden) re-arms the flag at its own chokepoint.
        // See Entity::HoldsEntityRefs.
        ClearHoldsEntityRefs();
    }

    void PrimedTnt::SetExplosionPower(float p) {
        // MC clamps on LOAD, not on set — but clamping here as well costs
        // nothing and stops a command or a bad save from asking for a radius
        // whose 16^3 ray march would take a visible fraction of the tick.
        m_explosionPower = std::clamp(p, 0.0f, kMaxPower);
    }

    void PrimedTnt::InitPrimed(const glm::dvec3& pos, Entity* owner) {
        position    = pos;
        oldPosition = pos;
        m_fuse      = kDefaultFuse;
        m_ownerRef.Set(owner);

        // MC: a uniformly random horizontal direction at a fixed tiny speed,
        // plus a fixed upward kick. Note the negated sin/cos — vanilla's, and
        // irrelevant to the distribution, but transcribed as written.
        if (m_level) {
            const double rot = m_level->Random().NextDouble() *
                               (2.0 * 3.14159265358979323846);
            velocity = glm::dvec3(-std::sin(rot) * kSpawnHorizontal,
                                  kSpawnVertical,
                                  -std::cos(rot) * kSpawnHorizontal);
        } else {
            velocity = glm::dvec3(0.0, kSpawnVertical, 0.0);
        }
    }

    Entity* PrimedTnt::GetOwner() {
        return m_level ? m_ownerRef.Get(*m_level) : nullptr;
    }

    void PrimedTnt::Tick() {
        // NO BaseTick() — MC's PrimedTnt.tick calls handlePortal(), not
        // super.tick(). The water-state refresh it does want arrives at the
        // BOTTOM of the tick via updateInWaterStateAndDoFluidPushing, and only
        // on a tick the fuse survives, which is the ordering reproduced below.
        firstTick = false;

        // ── Parked rest ─────────────────────────────────────────────────
        // A TNT that has settled (resting path zeroed its velocity) skips
        // its whole movement block until a block is written anywhere
        // (BlockWriteEpoch) or an impulse arrives (AddDeltaMovement clears
        // the flag). The mover is provably a no-op in that window — the
        // resting fast-path would re-probe the same unchanged cells and
        // zero the same velocity — and with a hundred thousand primed TNT
        // waiting out a fuse, those probes were most of both sides' tick.
        // On an epoch change the normal path below runs and re-parks.
        if (physicsParked) {
            // CLIENT: fully passive. The server is authoritative for a parked
            // TNT's rest — if the ground under it goes, the server's copy
            // falls and streams the movement, and the packet unparks this one
            // (see ClientMobManager). Checking the local stamp here cost a
            // million chunk lookups a tick for entities that cannot move on
            // their own authority anyway.
            //
            // SERVER: revalidate the columns' write stamp, staggered so a
            // quarter of the parked population checks each tick. Worst case a
            // block broken under a parked TNT is noticed 4 ticks (200 ms)
            // late; the blast that broke it usually flings the TNT the same
            // tick through AddDeltaMovement's unpark anyway.
            const bool clientSide = m_level && m_level->IsClientSide();
            if (clientSide ||
                ((static_cast<uint32_t>(tickCount) + static_cast<uint32_t>(GetId())) & 3u) != 0u ||
                (m_level && RestRegionStamp() == physicsParkedEpoch)) {
                TickFuseTail();
                return;
            }
            physicsParked = false;
        }

        // Client detail LOD — see Entity::clientPhysicsLod.
        if (m_level && m_level->IsClientSide() && clientPhysicsLod) {
            TickFuseTail();
            return;
        }

        ApplyGravity();
        // The server owns this position and streams it (ServerEntityTracker);
        // the client simulates only to fill the ticks between those packets, so
        // it takes the cheap mover. See Entity::MoveApproximate — one overlap
        // test in the airborne case against Move's ~16-27-cell swept gather,
        // which is the single biggest per-entity cost in the client tick when a
        // detonation puts a hundred thousand of these in the level.
        if (m_level && m_level->IsClientSide()) MoveApproximate(velocity);
        else                                   Move(velocity);

        // Drag BEFORE the ground damp — the opposite order to
        // FallingBlockEntity. See the header note; this is vanilla's.
        velocity *= kAirDrag;
        if (onGround) {
            velocity = glm::dvec3(velocity.x * kLandHorizontal,
                                  velocity.y * kLandVertical,
                                  velocity.z * kLandHorizontal);
        }

        // Settled? Snap the residual to the zero the server's resting path
        // produces anyway, and park. |vy| < 0.08 covers the gravity injected
        // above plus the damped landing bounce; horizontal must have decayed
        // to the resting path's own threshold.
        if (onGround && m_level &&
            std::abs(velocity.x) < 1.0e-5 && std::abs(velocity.z) < 1.0e-5 &&
            std::abs(velocity.y) < 0.08) {
            velocity = glm::dvec3(0.0);
            physicsParked      = true;
            physicsParkedEpoch = RestRegionStamp();
        }

        TickFuseTail();
    }

    // The non-movement half of the tick: fuse countdown, detonation hand-off,
    // water refresh and the client smoke plume. Split out so a parked TNT
    // (see above) can run it without the mover.
    // The write stamp of the columns this TNT's rest probe can touch (its
    // box plus a small margin). Parked-rest validity is judged against THIS,
    // not the global epoch: with a million settled TNT, one block broken
    // anywhere used to unpark all of them on every tick of a cascade.
    uint64_t PrimedTnt::RestRegionStamp() const {
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 0;
        const double half = GetBbWidth() * 0.5 + 0.1;
        const glm::ivec3 lo(static_cast<int>(std::floor(position.x - half)), 0,
                            static_cast<int>(std::floor(position.z - half)));
        const glm::ivec3 hi(static_cast<int>(std::floor(position.x + half)), 0,
                            static_cast<int>(std::floor(position.z + half)));
        return blocks->RegionWriteStamp(lo, hi);
    }

    void PrimedTnt::TickFuseTail() {
        --m_fuse;
        if (m_fuse <= 0) {
            // Hold at zero when the tick's explosion budget is spent, and ask
            // again next tick. See EntityLevel::TryBeginExplosion — this is a
            // deliberate divergence, and it is what keeps a mass detonation
            // from freezing the server for minutes at a stretch.
            //
            // Clamped so a long hold cannot wrap the fuse into a negative that
            // some other reader interprets as a live countdown.
            m_fuse = 0;
            if (m_level && !m_level->IsClientSide() && !m_level->TryBeginExplosion()) {
                return;
            }
            // CLIENT: hold at zero and wait for the server's removal. MC's
            // client discards here too, but MC's server also explodes here —
            // ours may hold the fuse under the explosion budget for many
            // ticks, and a client that discarded on its own showed the TNT
            // vanishing seconds before its blast. The fuse is not resent
            // (see AnimStateTicksOnClient), so nothing else would keep the
            // two in step.
            if (m_level && m_level->IsClientSide()) return;
            // MC discards FIRST and explodes second, so the blast's own entity
            // sweep does not find the TNT that produced it.
            Discard();
            if (m_level && !m_level->IsClientSide()) Explode();
            return;
        }

        // MC calls this on the surviving-fuse branch only, AFTER the movement
        // and damping. It is what lets a stream carry primed TNT downhill;
        // see Entity::UpdateInWaterStateAndDoFluidPushing for the flow half
        // that this engine cannot supply yet.
        UpdateInWaterStateAndDoFluidPushing();

        // MC's client-side plume: one smoke particle per surviving tick at the
        // entity's mid-height. Because it is gated on the fuse SURVIVING, the
        // final tick before the blast shows none.
        if (m_level && m_level->IsClientSide()) {
            m_level->AddParticle(ParticleKind::Smoke,
                                 position.x, position.y + 0.5, position.z,
                                 0.0, 0.0, 0.0);
        }
    }

    void PrimedTnt::Explode() {
        if (!m_level) return;

        ExplosionParams params;
        // MC getY(0.0625) — a sixteenth of the way up the entity, not its
        // centre. With a 0.98-tall box that is +0.06125, which is what puts a
        // TNT crater's floor one block lower than a centre-origin blast would.
        params.center = glm::dvec3(position.x,
                                   position.y + kExplodeHeightFrac * GetBbHeight(),
                                   position.z);
        params.radius = m_explosionPower;
        params.source = this;
        // Kill credit goes to whoever lit it, which is why the owner reference
        // survives a save — a TNT trap armed yesterday still names its author.
        params.attributedTo = GetOwner();
        params.interaction  = ExplosionInteraction::Tnt;
        params.fire         = false;   // MC passes false for TNT
        // MC PrimedTnt.explode: `usedPortal` swaps in a damage calculator that
        // gives NETHER_PORTAL and its obsidian frame infinite resistance. A TNT
        // that rode a portal through must not take the portal out behind it,
        // which would otherwise strand whatever follows it.
        params.sparePortalBlocks = m_usedPortal;

        // QUEUED, not exploded. The level batches a tick's blasts so their
        // crater scans — 245 us each, 54% of a detonating server tick — can run
        // across the worker pool instead of on it. The apply still happens this
        // same tick, in queue order, before MobManager sweeps this entity, so
        // `params.source` (this) is still alive when the apply reads it.
        m_level->QueueExplosion(params);
    }

} // namespace Game
