// File: src/common/entity/Entity.cpp
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    Entity::Entity(EntityTypeId type, EntityLevel* level)
        : m_type(type), m_level(level) {}

    // ── Riding ─────────────────────────────────────────────────────────────

    namespace {

        // MC EntityAttachments.transformPoint: point.yRot(-yRot * PI/180).
        // Expanded, that is the same frame Entity::GetInputVector uses:
        //   x' = x cos(yRot) - z sin(yRot),  z' = z cos(yRot) + x sin(yRot).
        glm::dvec3 RotateAttachmentY(const glm::dvec3& p, float yRotDeg) {
            const double a   = -static_cast<double>(yRotDeg) * Mth::kDegToRad;
            const double cos = std::cos(a);
            const double sin = std::sin(a);
            return glm::dvec3(p.x * cos + p.z * sin, p.y, p.z * cos - p.x * sin);
        }

        // MC EntityType.java `.passengerAttachments(...)` — where a rider sits,
        // relative to the vehicle's feet, before yaw rotation. Transcribed
        // verbatim from the decompile; MC keeps these on EntityType, but our
        // generated EntityTypeInfo has no attachment columns, so they live
        // here (see the note on Entity::GetPassengerAttachmentPoint).
        //
        // Returns false when the type has no explicit entry, in which case the
        // caller applies MC's fallback: (0, bbHeight, 0)
        // (EntityAttachment.Fallback.AT_HEIGHT). Types whose seat MC computes
        // in class code (Strider bobbing, Camel sitting, HappyGhast's four
        // seats) are deliberately absent — they get the fallback until their
        // mobs override the virtual.
        bool ExplicitPassengerAttachment(EntityTypeId type, glm::dvec3& out) {
            switch (type) {
                case EntityTypeId::Cat:             out = {0.0, 0.5125,   0.0};     return true;
                case EntityTypeId::Chicken:         out = {0.0, 0.7,     -0.1};     return true;
                case EntityTypeId::Cow:             out = {0.0, 1.36875,  0.0};     return true;
                case EntityTypeId::Donkey:          out = {0.0, 1.1125,   0.0};     return true;
                case EntityTypeId::Drowned:         out = {0.0, 2.0125,   0.0};     return true;
                case EntityTypeId::ElderGuardian:   out = {0.0, 2.350625, 0.0};     return true;
                case EntityTypeId::Enderman:        out = {0.0, 2.80625,  0.0};     return true;
                case EntityTypeId::Endermite:       out = {0.0, 0.2375,   0.0};     return true;
                case EntityTypeId::EnderDragon:     out = {0.0, 3.0,      0.0};     return true;
                case EntityTypeId::Evoker:          out = {0.0, 2.0,      0.0};     return true;
                case EntityTypeId::Fox:             out = {0.0, 0.6375,  -0.25};    return true;
                case EntityTypeId::Frog:            out = {0.0, 0.375,   -0.25};    return true;
                case EntityTypeId::Ghast:           out = {0.0, 4.0625,   0.0};     return true;
                case EntityTypeId::Goat:            out = {0.0, 1.1125,   0.0};     return true;
                case EntityTypeId::Guardian:        out = {0.0, 0.975,    0.0};     return true;
                case EntityTypeId::Hoglin:          out = {0.0, 1.49375,  0.0};     return true;
                case EntityTypeId::Horse:           out = {0.0, 1.44375,  0.0};     return true;
                case EntityTypeId::Husk:            out = {0.0, 2.075,    0.0};     return true;
                case EntityTypeId::Illusioner:      out = {0.0, 2.0,      0.0};     return true;
                case EntityTypeId::Llama:           out = {0.0, 1.37,    -0.3};     return true;
                case EntityTypeId::Mooshroom:       out = {0.0, 1.36875,  0.0};     return true;
                case EntityTypeId::Mule:            out = {0.0, 1.2125,   0.0};     return true;
                case EntityTypeId::Ocelot:          out = {0.0, 0.6375,   0.0};     return true;
                case EntityTypeId::Parrot:          out = {0.0, 0.4625,   0.0};     return true;
                case EntityTypeId::Phantom:         out = {0.0, 0.3375,   0.0};     return true;
                case EntityTypeId::Pig:             out = {0.0, 0.86875,  0.0};     return true;
                case EntityTypeId::Piglin:          out = {0.0, 2.0125,   0.0};     return true;
                case EntityTypeId::PiglinBrute:     out = {0.0, 2.0125,   0.0};     return true;
                case EntityTypeId::Pillager:        out = {0.0, 2.0,      0.0};     return true;
                case EntityTypeId::Rabbit:          out = {0.0, 0.5,      0.0};     return true;
                case EntityTypeId::Ravager:         out = {0.0, 2.2625,  -0.0625};  return true;
                case EntityTypeId::Sheep:           out = {0.0, 1.2375,   0.0};     return true;
                case EntityTypeId::Silverfish:      out = {0.0, 0.2375,   0.0};     return true;
                case EntityTypeId::SkeletonHorse:   out = {0.0, 1.31875,  0.0};     return true;
                case EntityTypeId::Sniffer:         out = {0.0, 2.09375,  0.0};     return true;
                case EntityTypeId::Spider:          out = {0.0, 0.765,    0.0};     return true;
                case EntityTypeId::TraderLlama:     out = {0.0, 1.37,    -0.3};     return true;
                case EntityTypeId::Zombie:          out = {0.0, 2.0125,   0.0};     return true;
                case EntityTypeId::ZombieHorse:     out = {0.0, 1.31875,  0.0};     return true;
                case EntityTypeId::ZombieVillager:  out = {0.0, 2.125,    0.0};     return true;
                case EntityTypeId::ZombifiedPiglin: out = {0.0, 2.0,      0.0};     return true;
                default: return false;
            }
        }

        // MC EntityType.java `.ridingOffset(f)`, which the builder stores as a
        // VEHICLE attachment of (0, -f, 0) — the values HERE are already the
        // attachment y (so zombie's ridingOffset(-0.7F) appears as +0.7: its
        // seated feet end up 0.7 below the seat point, which is what puts a
        // jockey's legs around the vehicle's body instead of on top of it).
        // Fallback for absent types is MC's AT_FEET: (0, 0, 0).
        double VehicleAttachmentY(EntityTypeId type) {
            switch (type) {
                case EntityTypeId::Bogged:          return 0.7;
                case EntityTypeId::Drowned:         return 0.7;
                case EntityTypeId::Evoker:          return 0.6;
                case EntityTypeId::Ghast:           return -0.5;
                case EntityTypeId::HappyGhast:      return -0.5;
                case EntityTypeId::Husk:            return 0.7;
                case EntityTypeId::Illusioner:      return 0.6;
                case EntityTypeId::Phantom:         return 0.125;
                case EntityTypeId::Piglin:          return 0.7;
                case EntityTypeId::PiglinBrute:     return 0.7;
                case EntityTypeId::Pillager:        return 0.6;
                case EntityTypeId::Skeleton:        return 0.7;
                case EntityTypeId::Stray:           return 0.7;
                case EntityTypeId::WitherSkeleton:  return 0.875;
                case EntityTypeId::Zombie:          return 0.7;
                case EntityTypeId::ZombieVillager:  return 0.7;
                case EntityTypeId::ZombifiedPiglin: return 0.7;
                default: return 0.0;
            }
        }

    } // namespace

    bool Entity::HasPassenger(const Entity& e) const {
        return std::find(m_passengers.begin(), m_passengers.end(), &e) !=
               m_passengers.end();
    }

    bool Entity::StartRiding(Entity& vehicle, bool force) {
        // MC Entity.startRiding(entityToRide, force, sendEventAndTriggers),
        // minus the pieces this port has no counterpart for: the game-event /
        // criteria broadcast and the canSerialize gate.
        if (&vehicle == m_vehicle) return false;
        if (!vehicle.CouldAcceptPassenger()) return false;

        // Cycle check — walking the vehicle chain upward must never reach us
        // (also refuses riding yourself, which MC's loop only catches once a
        // chain exists).
        for (Entity* v = &vehicle; v != nullptr; v = v->m_vehicle) {
            if (v == this) return false;
        }

        if (!force && !(CanRide(vehicle) && vehicle.CanAddPassenger(*this))) {
            return false;
        }

        if (IsPassenger()) StopRiding();

        SetPose(Pose::Standing);
        m_vehicle = &vehicle;
        // MC addPassenger. The player-first seat ordering is skipped: no
        // player can mount anything yet, so insertion order is boarding order.
        vehicle.m_passengers.push_back(this);
        // Both directions are now raw cross-entity pointers, so both ends have
        // to be visited by the pre-sweep. See Entity::HoldsEntityRefs.
        MarkHoldsEntityRefs();
        vehicle.MarkHoldsEntityRefs();

        // SIMPLIFICATION vs MC: the rider's fall state resets on mount. In MC
        // the stale value simply never settles (a passenger's move() does not
        // run); resetting is equivalent for damage — the vehicle's landing
        // propagates its OWN fallDistance — and avoids a bogus payout the
        // tick after a mid-air dismount.
        ResetFallDistance();
        return true;
    }

    void Entity::RemoveVehicle() {
        if (!m_vehicle) return;
        Entity* oldVehicle = m_vehicle;
        m_vehicle = nullptr;

        // MC removePassenger, folded in (it is only ever called from here).
        auto& seats = oldVehicle->m_passengers;
        seats.erase(std::remove(seats.begin(), seats.end(), this), seats.end());
        m_boardingCooldown = 60;
    }

    void Entity::EjectPassengers() {
        // Back to front, like MC — StopRiding erases from the vector.
        for (size_t i = m_passengers.size(); i-- > 0;) {
            m_passengers[i]->StopRiding();
        }
    }

    void Entity::RideTick() {
        // MC Entity.rideTick: the vehicle owns the rider's motion.
        velocity = glm::dvec3(0.0);
        Tick();
        if (IsPassenger()) {
            m_vehicle->PositionRider(*this);
        }
    }

    void Entity::PositionRider(Entity& passenger) {
        if (!HasPassenger(passenger)) return;
        const glm::dvec3 seat   = GetPassengerRidingPosition(passenger);
        const glm::dvec3 offset = passenger.GetVehicleAttachmentPoint();
        // MC positionRider's MoveFunction is Entity::setPos — a direct write,
        // no collision. `position` is feet, same as MC's.
        passenger.position = seat - offset;
    }

    glm::dvec3 Entity::GetPassengerRidingPosition(const Entity& passenger) const {
        return position + GetPassengerAttachmentPoint(passenger);
    }

    glm::dvec3 Entity::GetPassengerAttachmentPoint(const Entity& passenger) const {
        (void)passenger; // MC indexes multi-seat vehicles by passenger; every
                         // entry in the table is single-seat.
        glm::dvec3 local;
        if (!ExplicitPassengerAttachment(m_type, local)) {
            // MC EntityAttachment.Fallback.AT_HEIGHT.
            local = glm::dvec3(0.0, static_cast<double>(GetBbHeight()), 0.0);
        }
        return RotateAttachmentY(local, yRot);
    }

    glm::dvec3 Entity::GetVehicleAttachmentPoint() const {
        // Rotated by the RIDER's yaw, as in MC — a no-op for the (0, y, 0)
        // points in the table, kept for a future x/z entry.
        return RotateAttachmentY(glm::dvec3(0.0, VehicleAttachmentY(m_type), 0.0),
                                 yRot);
    }

    void Entity::UnlinkRidingReferenceTo(const Entity* dead) {
        if (m_vehicle == dead) m_vehicle = nullptr;
        m_passengers.erase(
            std::remove(m_passengers.begin(), m_passengers.end(), dead),
            m_passengers.end());
    }

    void Entity::Remove(RemovalReason reason) {
        // MC Entity.remove(reason): riders off, seat left, THEN the flag —
        // see the lifetime note on the riding block in the header.
        if (IsVehicle()) EjectPassengers();
        if (IsPassenger()) StopRiding();
        m_removal = reason;
    }

    void Entity::SetOldPosAndRot() {
        oldPosition = position;
        yRotO = yRot;
        xRotO = xRot;
    }

    glm::dvec3 Entity::GetInputVector(const glm::dvec3& input, float speed, float yRot) {
        const double lenSq = input.x * input.x + input.y * input.y + input.z * input.z;
        if (lenSq < 1.0e-7) return glm::dvec3(0.0);

        // MC normalises only when the input is LONGER than unit length. A
        // shorter input keeps its magnitude, which is how a mob that is only
        // partly turned toward its waypoint moves at less than full speed.
        glm::dvec3 movement = (lenSq > 1.0) ? input / std::sqrt(lenSq) : input;
        movement *= static_cast<double>(speed);

        const float sin = std::sin(yRot * Mth::kDegToRad);
        const float cos = std::cos(yRot * Mth::kDegToRad);
        return glm::dvec3(movement.x * cos - movement.z * sin,
                          movement.y,
                          movement.z * cos + movement.x * sin);
    }

    void Entity::MoveRelative(float speed, const glm::dvec3& input) {
        velocity += GetInputVector(input, speed, yRot);
    }

    void Entity::ApplyGravity() {
        velocity.y -= GetGravity();
    }

    void Entity::MoveApproximate(const glm::dvec3& delta) {
        if (!m_level) return;
        // The same rider seam as Move() — a passenger is placed by its vehicle.
        if (IsPassenger()) return;

        // The mover itself is Game::MoveApproximate (Physics.hpp), shared with the
        // compact falling-block store (see there for why it is a free
        // function). The server owns this position and streams it; the
        // client simulates only to fill the ticks between those packets, so it
        // takes this one-overlap-test mover instead of Move's ~16-27-cell
        // swept gather — the single biggest per-entity cost in the client
        // tick when a detonation puts a hundred thousand entities in the level.
        const PhysicsContext ctx = m_level->Physics();
        const bool verticalHit = ::Game::MoveApproximate(
            position, velocity, HalfExtents(), delta, onGround,
            horizontalCollision, verticalCollision, ctx);
        // CheckFallDamage is skipped with the rest of Move's tail (no
        // client-side damage), but its fallDistance reset is not optional:
        // the field only ever grows otherwise, and water and lava both
        // scale it.
        if (verticalHit) ResetFallDistance();
    }

    void Entity::Move(const glm::dvec3& delta) {
        if (!m_level) return;

        // ── THE RIDER SEAM ─────────────────────────────────────────────────
        // MC suppresses a passenger's self-movement structurally: ServerLevel
        // only calls tick() on NON-passengers, and rideTick zeroes
        // deltaMovement before the passenger's tick runs, so its travel moves
        // it nowhere. This port's managers mirror that control flow, but the
        // passenger's LivingEntity::Travel still executes inside RideTick's
        // Tick() and would apply one tick of gravity through here — so the
        // gate lives at the ONE choke point every self-movement path funnels
        // through, which conveniently needs no LivingEntity edit. The vehicle
        // places the rider via PositionRider instead; velocity pushes
        // (knockback) still accumulate and take effect on dismount.
        if (IsPassenger()) return;

        const PhysicsContext ctx = m_level->Physics();

        // MoveEntity consumes the velocity vector it is handed, zeroing blocked
        // axes. MC passes deltaMovement itself, so the caller's velocity is the
        // thing that must be mutated — hand it the real member, not a copy.
        const double oldY = position.y;
        glm::dvec3 vel = delta;
        const EntityMoveResult result =
            MoveEntity(position, vel, HalfExtents(), MaxUpStep(), onGround, ctx);

        // MC zeroes the collided components of deltaMovement, which for the
        // usual call (move(SELF, getDeltaMovement())) is the same vector. Copy
        // the resolved components back so a caller that passed something else
        // (knockback, piston push) still sees the correct post-move velocity.
        velocity = vel;

        onGround            = result.onGround;
        horizontalCollision = result.horizontalCollision;
        verticalCollision   = result.verticalCollision;

        // MC Entity.move -> checkFallDamage with the movement that actually
        // HAPPENED (post-collision), not the movement that was asked for.
        CheckFallDamage(position.y - oldY, onGround);

        // MC Entity.move's tail: velocity *= (blockSpeedFactor, 1,
        // blockSpeedFactor). getBlockSpeedFactor reads the block AT the
        // position (water excepted — its factor is 1 and MC skips the
        // fallback there; no bubble columns here), falling back to the
        // movement-affecting block below when it is 1.0. Only soul sand and
        // honey override the default (0.4 each) — the slowdown a soul-sand
        // corridor or honey trap applies to every mob walking it.
        if (onGround)
        if (const IBlockAccess* blocks = m_level->Blocks()) {
            const auto factorOf = [](BlockID id) {
                return (id == BlockID::SoulSand || id == BlockID::HoneyBlock)
                    ? 0.4f : 1.0f;
            };
            const glm::ivec3 p = BlockPosition();
            const BlockID here = blocks->GetBlock(p.x, p.y, p.z);
            float speedFactor = factorOf(here);
            if (here != BlockID::Water && speedFactor == 1.0f) {
                // MC getBlockPosBelowThatAffectsMyMovement — getOnPos(0.500001).
                const int belowY =
                    static_cast<int>(std::floor(position.y - 0.500001));
                speedFactor = factorOf(blocks->GetBlock(p.x, belowY, p.z));
            }
            if (speedFactor != 1.0f) {
                velocity.x *= speedFactor;
                velocity.z *= speedFactor;
            }
        }
    }

    void Entity::CheckFallDamage(double dy, bool onGroundNow) {
        // MC Entity.checkFallDamage:1481-1495. The accumulation is
        // UNCONDITIONAL and runs BEFORE the onGround test — it is not an
        // `else` branch. That ordering is load-bearing: on the tick the
        // entity actually lands, `dy` is the final CLIPPED descent, and
        // folding it in is what makes an N-block drop measure N. Hanging the
        // accumulation off `else if (!onGround)` silently drops that last
        // partial block, and because CalculateFallDamage floors, the loss
        // lands whole on the damage number — a 23-block drop dealt 18 instead
        // of 20, so the canonical mob-farm height stopped killing.
        if (!IsInWater() && dy < 0.0) {
            fallDistance -= static_cast<float>(dy);
        }

        if (onGroundNow) {
            if (fallDistance > 0.0f) {
                // MC Block.fallOn's default: causeFallDamage(fd, 1.0F). The
                // per-block multipliers (hay 0.2, beds 0.5-off bounce, slime 0)
                // arrive with those blocks' behaviours.
                //
                // Riders take the landing too — MC does this inside
                // causeFallDamage itself; see PropagateFallToPassengers's
                // header note for why it is called from here instead.
                PropagateFallToPassengers(fallDistance, 1.0f);
                CauseFallDamage(fallDistance, 1.0f);
            }
            // MC resets unconditionally inside the onGround branch.
            ResetFallDistance();
        }
    }

    void Entity::PropagateFallToPassengers(double fallDist, float damageMultiplier) {
        if (!IsVehicle()) return;
        // Copy: CauseFallDamage can kill a rider, whose Remove() edits the
        // passenger list mid-walk.
        const std::vector<Entity*> riders = m_passengers;
        for (Entity* rider : riders) {
            rider->CauseFallDamage(fallDist, damageMultiplier);
        }
    }

    bool Entity::IsInWater() const {
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;

        // MC tests the fluid the entity's box overlaps. This engine has no
        // fluid-height model, so the test is "is the block at the entity's eye-
        // low / feet-high midpoint a fluid" — accurate enough for the two
        // things that read it (FloatGoal and the water movement branch) and
        // deliberately coarse rather than pretending to a precision the block
        // data cannot support.
        const glm::ivec3 p = BlockPosition();
        return blocks->IsBlockFluid(p.x, p.y, p.z);
    }

    bool Entity::IsEyeInWater() const {
        // MC isEyeInFluid samples the fluid HEIGHT at the eye block; with no
        // fluid-height model the test is "is the block containing the eye
        // water", the same coarseness IsInWater documents.
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;
        const int ex = static_cast<int>(std::floor(position.x));
        const int ey = static_cast<int>(std::floor(GetEyeY()));
        const int ez = static_cast<int>(std::floor(position.z));
        return blocks->ContainsWater(ex, ey, ez);
    }

    // MC Entity.checkInsideBlocks (Entity.java:1240-1309)
    void Entity::CheckInsideBlocks(ILevelWrite& level) {
        // MC deflates the box by 1e-5 before choosing cells so that an entity
        // resting exactly on a boundary does not claim the cell it is merely
        // touching. The constant here is 0.001, which is coarser than MC but is
        // what the rest of this engine's in-block tests already agree on. Using
        // MC's value instead would make a player standing flush against a
        // portal's face count as inside it while the water test said otherwise.
        constexpr double kShrink = 0.001;

        // THE SWEEP, and why it is not optional.
        //
        // MC walks every block the entity's box passed THROUGH between last
        // position and this one (Entity.checkInsideBlocks:1240 ->
        // forEachBlockIntersectedBetween(from, to, ...)). Sampling only the
        // destination — which is what this did first — works fine for anything
        // that walks, and fails completely for anything faster than its own
        // hitbox: a nether portal's shape is 4/16 of a block thick, so a
        // creative player flying at 2 blocks a tick crosses the entire portal
        // between two samples and is never once seen inside it. That is exactly
        // the "portals do nothing in creative but work in survival" symptom.
        //
        // The step is a quarter block — smaller than the thinnest shape any
        // block here has — so nothing can be skipped over.
        const glm::dvec3 from = oldPosition;
        const glm::dvec3 to   = position;
        const glm::dvec3 delta = to - from;
        const double distance = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                          delta.z * delta.z);

        // A jump this large is a teleport, not movement — after a dimension
        // change `from` is in another world entirely. MC resets its tracking on
        // teleport; here it is enough to sample only where the entity landed,
        // which also stops a portal arrival from re-triggering the portal it
        // came out of by sweeping the line back to the entry.
        constexpr double kTeleportDistance = 8.0;
        constexpr int    kMaxSteps = 16;

        int steps = 1;
        if (distance > 1.0e-6 && distance <= kTeleportDistance) {
            steps = static_cast<int>(std::ceil(distance / 0.25)) + 1;
            if (steps > kMaxSteps) steps = kMaxSteps;
        }

        // MC's `visitedBlocks` — entityInside fires at most once per block per
        // tick however many steps pass through it.
        m_insideBlocksScratch.clear();

        const double w = GetBbWidth(), h = GetBbHeight();

        // Whole-sweep early-out: if every cell the swept box can touch lies in
        // an all-air section there is nothing to dispatch, and the per-step,
        // per-cell reads below are skipped outright. This is what makes
        // TickPortals affordable with a hundred thousand airborne primed TNT —
        // one section-flag test each instead of 8-40 state reads. (TickPortals
        // asks the same question through SweptInsideRegion from its parallel
        // prefilter, so for the entities it forwards this is a repeat of a
        // test that already said "not all air".)
        {
            glm::ivec3 cLo, cHi;
            SweptInsideRegion(cLo, cHi);
            if (level.IsRegionAllAir(cLo, cHi)) return;
        }

        for (int step = 0; step < steps; ++step) {
            // Walk from `from` to `to`, ending exactly on `to` so the
            // destination is always sampled.
            const double t = (steps == 1) ? 1.0
                                          : static_cast<double>(step) / (steps - 1);
            const glm::dvec3 feet = from + delta * t;

            const AABBd box{
                glm::dvec3(feet.x - w * 0.5, feet.y,     feet.z - w * 0.5),
                glm::dvec3(feet.x + w * 0.5, feet.y + h, feet.z + w * 0.5)
            };

            const int minX = static_cast<int>(std::floor(box.min.x + kShrink));
            const int minY = static_cast<int>(std::floor(box.min.y + kShrink));
            const int minZ = static_cast<int>(std::floor(box.min.z + kShrink));
            const int maxX = static_cast<int>(std::floor(box.max.x - kShrink));
            const int maxY = static_cast<int>(std::floor(box.max.y - kShrink));
            const int maxZ = static_cast<int>(std::floor(box.max.z - kShrink));

            // One bulk read per step box (a TNT's is 2x2x2 = 8 cells): one
            // chunk and one section lookup instead of one of each per cell.
            thread_local std::vector<BlockState> t_stepStates;
            const int ny = maxY - minY + 1, nz = maxZ - minZ + 1;
            if (ny <= 0 || nz <= 0 || maxX < minX) continue;
            t_stepStates.resize(static_cast<size_t>(maxX - minX + 1) * ny * nz);
            level.GetBlockStatesInBox(glm::ivec3(minX, minY, minZ),
                                      glm::ivec3(maxX, maxY, maxZ), t_stepStates.data());

            for (int y = minY; y <= maxY; ++y) {
                for (int z = minZ; z <= maxZ; ++z) {
                    for (int x = minX; x <= maxX; ++x) {
                        const BlockState state = t_stepStates[(static_cast<size_t>(x - minX) * ny + (y - minY)) * nz + (z - minZ)];
                        // Air first, and by BlockID rather than through the
                        // registry. Practically every cell an entity sweeps is
                        // air, and air can never have an entityInside hook — so
                        // this skips the registry lookup and the shape-set
                        // fetch below on the path taken ~100% of the time.
                        // Exactly equivalent: BlockRegistry::Get(Air)
                        // .entityInside is null, so the old code continued here
                        // too, just after doing the work first.
                        //
                        // It matters because TickPortals calls this for EVERY
                        // entity EVERY tick, and at a hundred thousand primed
                        // TNT that is ~2.4 million cells a tick — 35.7 ms, 17%
                        // of the server tick, spent establishing that there is
                        // no portal.
                        if (state.Block() == BlockID::Air) continue;
                        const Block& def = BlockRegistry::Get(state.Block());
                        if (!def.entityInside) continue;

                        // MC tests the entity's box against the block's SHAPE,
                        // not its cell — and does so even for blocks with no
                        // collision, which is the whole point here: a nether
                        // portal is 4/16 thin on its short axis, so standing in
                        // the cell in front of one must not count as standing
                        // in it.
                        const auto shapes = BlockRegistry::GetBlockShapeSet(state);
                        bool overlaps = false;
                        for (const auto& sh : shapes) {
                            if (box.max.x - kShrink > x + sh.min.x &&
                                box.min.x + kShrink < x + sh.max.x &&
                                box.max.y - kShrink > y + sh.min.y &&
                                box.min.y + kShrink < y + sh.max.y &&
                                box.max.z - kShrink > z + sh.min.z &&
                                box.min.z + kShrink < z + sh.max.z) {
                                overlaps = true;
                                break;
                            }
                        }
                        if (!overlaps) continue;

                        const glm::ivec3 cell(x, y, z);
                        bool seen = false;
                        for (const glm::ivec3& v : m_insideBlocksScratch) {
                            if (v == cell) { seen = true; break; }
                        }
                        if (seen) continue;
                        m_insideBlocksScratch.push_back(cell);

                        def.entityInside(level, cell, state, *this);
                    }
                }
            }
        }
    }

    void Entity::SweptInsideRegion(glm::ivec3& outLo, glm::ivec3& outHi) const {
        // Same constant CheckInsideBlocks uses — see its header note.
        constexpr double kShrink = 0.001;
        const double w = GetBbWidth(), h = GetBbHeight();
        const glm::dvec3 lo = glm::min(oldPosition, position);
        const glm::dvec3 hi = glm::max(oldPosition, position);
        outLo = glm::ivec3(static_cast<int>(std::floor(lo.x - w * 0.5 + kShrink)),
                           static_cast<int>(std::floor(lo.y + kShrink)),
                           static_cast<int>(std::floor(lo.z - w * 0.5 + kShrink)));
        outHi = glm::ivec3(static_cast<int>(std::floor(hi.x + w * 0.5 - kShrink)),
                           static_cast<int>(std::floor(hi.y + h - kShrink)),
                           static_cast<int>(std::floor(hi.z + w * 0.5 - kShrink)));
    }

    void Entity::IgniteForSeconds(int seconds) {
        const int ticks = seconds * 20;
        if (ticks > m_remainingFireTicks) m_remainingFireTicks = ticks;
    }

    void Entity::BaseTick() {
        firstTick = false;

        // MC Entity.baseTick: a passenger whose vehicle is gone dismounts.
        // Mostly redundant with Remove()'s eject, but it is MC's own safety
        // net and catches a vehicle flagged removed between eject and sweep.
        if (IsPassenger() && GetVehicle()->IsRemoved()) {
            StopRiding();
        }
        if (m_boardingCooldown > 0) {
            --m_boardingCooldown;
        }

        if (m_remainingFireTicks > 0) {
            --m_remainingFireTicks;
        }

        UpdateInWaterStateAndDoFluidPushing();

        // MC Entity.checkBelowWorld — anything that falls out of the world is
        // discarded rather than left falling forever. The threshold is the
        // world floor minus 64, matching MC.
        if (position.y < -64.0 - 64.0) {
            Discard();
        }
    }

    void Entity::UpdateInWaterStateAndDoFluidPushing() {
        // MC updateInWaterStateAndDoFluidPushing resets fall distance on water
        // contact; lava only HALVES it per tick (Entity.java lava handling), so
        // a lava landing still hurts.
        if (IsInWater()) {
            ResetFallDistance();
        } else if (IsInLava()) {
            fallDistance *= 0.5f;
        }

        // NOT IMPLEMENTED — MC's current push. See the header note: it reads
        // FluidState.getFlow per overlapped cell, averages the vectors,
        // normalises and scales by 0.014 (water) / 0.007 (lava), and this
        // engine has no fluid level or flow direction to read. Everything that
        // should drift downstream — primed TNT, dropped items, boats, the
        // player — instead sits still in a river.
    }

    void Entity::Tick() {
        BaseTick();
    }

    void SpawnExplosionVisualEffects(EntityLevel& level,
                                     double cx, double cy, double cz,
                                     float radius, bool small, int blockCount) {
        if (!level.IsClientSide()) return;

        // MC ClientPacketListener.handleExplosion: the centre particle.
        // xAux = 1.0 travels into HugeExplosionParticle's `size` argument
        // (quadSize = 2 * (1 - size * 0.5) → 1.0 for the packet spawn).
        level.AddParticle(small ? ParticleKind::Explosion
                                : ParticleKind::ExplosionEmitter,
                          cx, cy, cz, 1.0, 0.0, 0.0);

        // MC ClientExplosionTracker:26 — anything but Particles: All clears
        // the whole debris list; only the fireball above survives.
        if (level.GetParticleStatus() != ParticleStatus::All) return;

        // MC ClientExplosionTracker.addParticle × min(blockCount, 512):
        // random direction, radius cbrt-distributed within the blast, only
        // where the world is air, speed falling off with distance. The
        // poof/smoke split is Level.DEFAULT_EXPLOSION_BLOCK_PARTICLES —
        // POOF (position scaling 0.5) and SMOKE (scaling 1.0) at equal
        // weight, both speed 1.0.
        JavaRandom& rng = level.Random();
        const IBlockAccess* blocks = level.Blocks();
        const int count = std::min(blockCount, 512);
        for (int i = 0; i < count; ++i) {
            const double dirX = static_cast<double>(rng.NextFloat() * 2.0f - 1.0f);
            const double dirY = static_cast<double>(rng.NextFloat() * 2.0f - 1.0f);
            const double dirZ = static_cast<double>(rng.NextFloat() * 2.0f - 1.0f);
            const double len = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
            if (len < 1.0e-9) continue;   // Vec3.normalize gives ZERO; skip
            const glm::dvec3 dir(dirX / len, dirY / len, dirZ / len);
            const float r = static_cast<float>(
                std::cbrt(static_cast<double>(rng.NextFloat()))) * radius;
            const glm::dvec3 pos = glm::dvec3(cx, cy, cz) + dir * static_cast<double>(r);
            const bool isAir = blocks == nullptr ||
                blocks->GetBlock(static_cast<int>(std::floor(pos.x)),
                                 static_cast<int>(std::floor(pos.y)),
                                 static_cast<int>(std::floor(pos.z))) == BlockID::Air;
            if (!isAir) continue;
            const float speed =
                0.5f / (r / radius + 0.1f) * rng.NextFloat() * rng.NextFloat() + 0.3f;
            const bool poof = rng.NextInt(2) == 0;   // equal-weight WeightedList
            const double scaling = poof ? 0.5 : 1.0;  // ExplosionParticleInfo.scaling
            const glm::dvec3 particlePos =
                glm::dvec3(cx, cy, cz) + dir * (static_cast<double>(r) * scaling);
            const glm::dvec3 vel = dir * static_cast<double>(speed);  // info.speed = 1.0
            level.AddParticle(poof ? ParticleKind::Poof : ParticleKind::Smoke,
                              particlePos.x, particlePos.y, particlePos.z,
                              vel.x, vel.y, vel.z);
        }
    }

    // Out of line so the by-value unique_ptr parameter is destroyed where
    // Entity is complete (see EntityLevel.hpp). The base level takes no
    // ownership: the client has no entity list, the server overrides.
    void EntityLevel::AddFreshEntity(std::unique_ptr<Entity> entity) {
        (void)entity;
    }

} // namespace Game

