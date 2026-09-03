// File: src/common/entity/FallingBlockEntity.cpp
#include "common/entity/FallingBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/FallingBlockLanding.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/FallingBlock.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    FallingBlockEntity::FallingBlockEntity(EntityLevel* level)
        : Mob(EntityTypeId::FallingBlock, level, NoAiTag{}) {

        // Opt out of the dying-mob reference sweep. This type registers no
        // goals, has no brain, never calls SetTarget and has no
        // ClearReferenceTo override, so that pass is a provable no-op for it —
        // and a mass detonation puts a million of these in the level, where the
        // pass is O(dying x surviving). Any real acquisition (being hurt,
        // riding, being ridden) re-arms the flag at its own chokepoint.
        // See Entity::HoldsEntityRefs.
        ClearHoldsEntityRefs();

        // MC sets blocksBuilding = true; this engine has no such flag, and the
        // consequence (you cannot place a block inside a falling one) falls out
        // of the placement collision test anyway.
        //
        // Health is left at LivingEntity's default and never read: Hurt is
        // overridden to refuse, and nothing else can reduce it.

        // MC FallingBlockEntity.DEFAULT_BLOCK_STATE = Blocks.SAND
        // .defaultBlockState(). Not air: tick() discards immediately on an air
        // state, so a falling block created without a carried state — a bare
        // `/summon falling_block`, or a save whose BlockState tag is missing —
        // would vanish the instant it spawned instead of falling as sand.
        m_blockState = BlockStates::Default(BlockID::Sand);
    }

    void FallingBlockEntity::InitFall(const glm::ivec3& blockPos, BlockState state) {
        m_blockState = state;
        m_startPos   = blockPos;
        m_time       = 0;

        // MC: (x + 0.5, y, z + 0.5) — horizontally centred, but NOT vertically
        // offset. `position` is the entity's feet, so a block spawned at its
        // own cell's y occupies exactly the cell it left.
        position = glm::dvec3(static_cast<double>(blockPos.x) + 0.5,
                              static_cast<double>(blockPos.y),
                              static_cast<double>(blockPos.z) + 0.5);
        oldPosition = position;
        velocity    = glm::dvec3(0.0);
        onGround    = false;
    }

    void FallingBlockEntity::Tick() {
        if (TickPhysics() != PhysicsOutcome::NeedsLanding) return;
        TickLanding(nullptr);
    }

    FallingBlockEntity::PhysicsSnapshot FallingBlockEntity::CapturePhysics() const {
        PhysicsSnapshot snap;
        snap.position            = position;
        snap.velocity            = velocity;
        snap.half                = HalfExtents();
        snap.gravity             = GetGravity();
        snap.fallDistance        = fallDistance;
        snap.onGround            = onGround;
        snap.horizontalCollision = horizontalCollision;
        snap.verticalCollision   = verticalCollision;
        snap.time                = m_time;
        return snap;
    }

    void FallingBlockEntity::RestorePhysics(const PhysicsSnapshot& snap) {
        position            = snap.position;
        velocity            = snap.velocity;
        fallDistance        = snap.fallDistance;
        onGround            = snap.onGround;
        horizontalCollision = snap.horizontalCollision;
        verticalCollision   = snap.verticalCollision;
        m_time              = snap.time;
    }

    FallingBlockEntity::PhysicsOutcome FallingBlockEntity::TickPhysics() {
        // MC: an air state means the block was somehow lost; nothing to fall.
        if (m_blockState.Block() == BlockID::Air) {
            Discard();
            return PhysicsOutcome::Discarded;
        }

        // NO BaseTick() — and that omission is MC's, not an oversight.
        // FallingBlockEntity.tick() deliberately does not call super.tick();
        // it runs applyEffectsFromBlocks() and handlePortal() instead. The
        // base tick's `if (isInWater()) resetFallDistance()` is the reason it
        // matters: inheriting it made an anvil that clipped so much as one
        // tick of water land for ZERO damage where vanilla deals full. It also
        // drags in checkBelowWorld, which would discard a void-bound falling
        // block without its item — a path MC's falling block cannot reach,
        // because the out-of-world despawn below owns that case.
        ++m_time;

        ApplyGravity();
        // The server owns this position and streams it (ServerEntityTracker);
        // the client simulates only to fill the ticks between those packets, so
        // it takes the cheap mover. See Entity::MoveApproximate — one overlap
        // test in the airborne case against Move's ~16-27-cell swept gather,
        // which is the single biggest per-entity cost in the client tick when a
        // detonation puts a hundred thousand of these in the level.
        if (m_level && m_level->IsClientSide()) MoveApproximate(velocity);
        else                                   Move(velocity);

        if (m_level && !m_level->IsClientSide() && IsAlive()) {
            // The airborne fast path. For an entity that is neither on the
            // ground nor concrete powder, the landing half would do exactly
            // two things: the expiry test, and the drag. Both read and write
            // only this entity, so they run here and the serial half skips it
            // — UNLESS it is expiring, because that path spawns an item and
            // discards, which stay serial. Concrete powder always takes the
            // serial half: its stop condition is not onGround, and its swept
            // water clip reads more of the world.
            if (!onGround && !IsConcretePowder(m_blockState.Block())) {
                const glm::ivec3 pos = BlockPosition();
                const bool outOfWorld =
                    pos.y <= m_level->GetMinY() || pos.y > m_level->GetMaxY();
                const bool expiring =
                    (m_time > kOutOfWorldGrace && outOfWorld) || m_time > kMaxLifetime;
                if (!expiring) {
                    velocity *= kAirDrag;   // the same "always last" the landing half does
                    return PhysicsOutcome::Airborne;
                }
            }
            return PhysicsOutcome::NeedsLanding;
        }

        // Client, or already removed: no landing branch exists, only the drag.
        velocity *= kAirDrag;
        return PhysicsOutcome::Airborne;
    }

    bool FallingBlockEntity::TickLanding(glm::ivec3* outLandingCell) {
        bool landed = false;
        if (m_level && !m_level->IsClientSide() && IsAlive()) {
            glm::ivec3 pos = BlockPosition();

            const bool isConcrete = IsConcretePowder(m_blockState.Block());
            const IBlockAccess* blocks = m_level->Blocks();
            bool stuckInWater = isConcrete && blocks &&
                BlockRegistry::ContainsWater(
                    blocks->GetBlockState(pos.x, pos.y, pos.z));

            // MC's concrete-powder swept clip. Powder moving more than a block
            // per tick can pass clean THROUGH a thin sheet of water between two
            // ticks, so vanilla re-clips the segment it actually travelled and
            // solidifies at the first water it crossed. The clip is
            // ClipContext.Block.COLLIDER + Fluid.SOURCE_ONLY, so it stops on
            // the water rather than sailing through it to the floor.
            if (isConcrete && blocks && glm::dot(velocity, velocity) > 1.0) {
                glm::ivec3 hit;
                if (ClipBlocksCollider(*blocks, oldPosition, position, hit,
                                       /*includeWaterSource=*/true) &&
                    BlockRegistry::ContainsWater(
                        blocks->GetBlockState(hit.x, hit.y, hit.z))) {
                    pos = hit;
                    stuckInWater = true;
                }
            }

            if (!onGround && !stuckInWater) {
                // MC: out of the world for long enough, or simply too old.
                // The bounds are the DIMENSION's, not the overworld's — the
                // End's floor is y=0, so a block falling off the island must
                // give up 64 blocks sooner than World::MIN_Y would say.
                const bool outOfWorld =
                    pos.y <= m_level->GetMinY() || pos.y > m_level->GetMaxY();
                if ((m_time > kOutOfWorldGrace && outOfWorld) || m_time > kMaxLifetime) {
                    if (m_dropItem && m_level->DoEntityDrops()) DropAsItem();
                    Discard();
                }
            } else {
                // MC damps BEFORE trying to place, so a block that fails to
                // place and pops as an item has already lost its momentum.
                velocity = glm::dvec3(velocity.x * kLandHorizontal,
                                      velocity.y * kLandVertical,
                                      velocity.z * kLandHorizontal);
                if (outLandingCell) *outLandingCell = pos;
                landed = true;
                TryLand(pos);
            }
        }

        // ALWAYS last, outside every branch — see the header note. PrimedTnt
        // applies this BEFORE its ground damp, and the difference is real.
        velocity *= kAirDrag;
        return landed;
    }

    void FallingBlockEntity::TryLand(const glm::ivec3& pos) {
        ILevelWrite* level = m_level ? m_level->MutableBlocks() : nullptr;
        if (!level) return;

        // The logic lives in FallingBlockLanding.cpp, shared with the compact
        // FallingBlockStore representation so the two cannot disagree about
        // what landing means. Only the outcome's effect on THIS entity is
        // decided here: everything but Retry discards it (MC's refused-write
        // path with drops off leaves the entity to try again next tick).
        const FallingBlockLandOutcome outcome = FallingBlockTryLand(
            *level, pos, m_blockState, position, m_dropItem, m_cancelDrop,
            m_level->DoEntityDrops());
        if (outcome != FallingBlockLandOutcome::Retry) Discard();
    }

    void FallingBlockEntity::DropAsItem() {
        // MC spawnAtLocation(block) — the BLOCK ITEM, not the loot table. A
        // falling block that cannot land drops ITSELF, even for a block whose
        // loot table would normally give something else (gravel never flints
        // here, and suspicious sand never yields its buried treasure).
        const BlockID id = m_blockState.Block();
        if (id == BlockID::Air) return;
        // MC spawnAtLocation drops at the ENTITY's exact position. Popping it
        // from BlockPosition() instead snapped the item to the centre of
        // whatever cell the block happened to be overlapping, which reads as a
        // visible sideways jump at the moment a falling block gives up.
        DropItemStackAt(m_level ? m_level->Dimension() : DimensionId::Overworld,
                        position, ItemStack(id, 1));
    }

    bool FallingBlockEntity::CauseFallDamage(double fallDist, float /*damageMultiplier*/) {
        // MC causeFallDamage: only armed blocks bite, and the distance is
        // rounded UP after subtracting the free first block.
        if (!m_hurtEntities || !m_level) return false;

        const int distance = static_cast<int>(std::ceil(fallDist - 1.0));
        if (distance < 0) return false;

        const float damage = std::min(
            std::floor(static_cast<float>(distance) * m_fallDamagePerDistance),
            static_cast<float>(m_fallDamageMax));

        // MC hurts everything inside the falling block's OWN bounding box —
        // this is a crush, not a blast, so standing beside an anvil is safe.
        std::vector<Entity*> hit;
        m_level->GetEntitiesInBox(GetAABB(), this, hit);
        const MobDamageSource source = IsAnvil(m_blockState.Block())
            ? MobDamageSource::FallingAnvil
            : MobDamageSource::FallingBlock;
        for (Entity* e : hit) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            if (living->IsCreative() || living->IsSpectator()) continue;
            living->Hurt(source, damage, this);
        }

        // MC: the anvil chips with probability 0.05 + distance * 0.05, and a
        // damaged anvil that chips again is destroyed outright (cancelDrop).
        if (IsAnvil(m_blockState.Block()) && damage > 0.0f) {
            JavaRandom& rng = m_level->Random();
            if (rng.NextFloat() < 0.05f + static_cast<float>(distance) * 0.05f) {
                const BlockID next = AnvilDamaged(m_blockState.Block());
                if (next == BlockID::Air) {
                    m_cancelDrop = true;
                } else {
                    // Keep the FACING: a chipped anvil must point the same way
                    // the anvil did, or it visibly snaps round on impact.
                    const auto& oldDef =
                        BlockRegistry::GetStateDefinition(m_blockState.Block());
                    const auto& newDef = BlockRegistry::GetStateDefinition(next);
                    BlockRegistry::BlockStateDefinition::PropertyMap props;
                    const std::string_view facing =
                        oldDef.ValueOf(m_blockState.Index(), "facing");
                    if (!facing.empty()) props["facing"] = std::string(facing);
                    m_blockState = BlockStates::FromIndex(next, newDef.IndexOf(props));
                }
            }
        }

        // MC returns false: the falling block itself takes no fall damage.
        return false;
    }

} // namespace Game
