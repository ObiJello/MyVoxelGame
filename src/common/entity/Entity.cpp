// File: src/common/entity/Entity.cpp
#include "common/entity/Entity.hpp"
#include "common/world/block/BlockBounce.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Log.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/sound/EntitySounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <algorithm>
#include <cmath>
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include <glm/geometric.hpp>

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
        // MC EntityCollisionContext: the Aether's aerclouds shape by the
        // mover's fall distance (AercloudBlock.hpp). No mob glides here.
        PhysicsContext ctx = m_level->Physics();
        ctx.collisionEntity       = true;
        ctx.collisionFallDistance = fallDistance;
        const bool verticalHit = ::Game::MoveApproximate(
            position, velocity, HalfExtents(), delta, onGround,
            horizontalCollision, verticalCollision, ctx);
        // CheckFallDamage is skipped with the rest of Move's tail (no
        // client-side damage), but its fallDistance reset is not optional:
        // the field only ever grows otherwise, and water and lava both
        // scale it.
        if (verticalHit) ResetFallDistance();
    }

    namespace {
        // The two block tags the mob footstep reads, resolved per BlockID
        // once from the registry slugs:
        //   #climbable + powder snow — MC isStateClimbable: a climbing mob's
        //     odometer runs on its full movement, not just the horizontal;
        //   #crystal_sound_blocks — amethyst chimes as it is walked on.
        struct StepTags {
            std::vector<uint8_t> climbable;
            std::vector<uint8_t> crystal;
        };
        const StepTags& GetStepTags() {
            static const StepTags tags = [] {
                StepTags t;
                t.climbable.assign(BlockRegistry::Size, 0);
                t.crystal.assign(BlockRegistry::Size, 0);
                static constexpr std::string_view kClimbable[] = {
                    "ladder", "vine", "scaffolding", "weeping_vines", "weeping_vines_plant",
                    "twisting_vines", "twisting_vines_plant", "cave_vines", "cave_vines_plant",
                    "powder_snow"};
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    for (std::string_view c : kClimbable) if (slug == c) t.climbable[i] = 1;
                    if (slug == "amethyst_block" || slug == "budding_amethyst") t.crystal[i] = 1;
                }
                return t;
            }();
            return tags;
        }
        bool IsClimbableForSound(BlockID id) {
            const size_t i = static_cast<size_t>(id);
            const auto& t = GetStepTags().climbable;
            return i < t.size() && t[i];
        }
        bool IsCrystalSoundBlock(BlockID id) {
            const size_t i = static_cast<size_t>(id);
            const auto& t = GetStepTags().crystal;
            return i < t.size() && t[i];
        }
    } // namespace

    SoundSource Entity::GetSoundSource() const {
        return EntitySoundsOf(GetType()).source;
    }

    const char* Entity::GetSwimSound() const { return EntitySoundsOf(GetType()).swim; }
    const char* Entity::GetSwimSplashSound() const { return EntitySoundsOf(GetType()).splash; }
    const char* Entity::GetSwimHighSpeedSplashSound() const { return EntitySoundsOf(GetType()).splashHighSpeed; }

    bool Entity::EmitsMovementSounds() const {
        return EntitySoundsOf(GetType()).stepMode != EntityStepMode::None;
    }

    void Entity::PlayStepSound(const glm::ivec3& pos, BlockState state) {
        (void)pos;
        const EntitySoundRow& row = EntitySoundsOf(GetType());
        switch (row.stepMode) {
            case EntityStepMode::None:
                return;
            case EntityStepMode::Event:
                // The class's own playStepSound (a zombie's shuffle, a
                // spider's skitter).
                PlaySound(PickSound(row.step, *this), row.stepVolume, row.stepPitch);
                return;
            case EntityStepMode::Block: {
                // MC Entity.playStepSound: the block's SoundType step at
                // 0.15 × its volume, its pitch.
                const SoundType& type = SoundTypeOf(state);
                PlaySound(type.GetStepSound(), type.GetVolume() * 0.15f, type.GetPitch());
                return;
            }
        }
    }

    void Entity::PlayEntityOnFireExtinguishedSound() {
        // MC: server only, NULL except, this entity's category.
        if (!m_level || m_level->IsClientSide()) return;
        JavaRandom& rng = m_level->Random();
        const float pitch = 1.6f + (rng.NextFloat() - rng.NextFloat()) * 0.4f;
        m_level->PlaySound(nullptr, position, SoundEvents::GENERIC_EXTINGUISH_FIRE, GetSoundSource(), 0.7f, pitch);
    }

    void Entity::PlaySwimSound(float volume) {
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        PlaySound(GetSwimSound(), volume, 1.0f + (rng.NextFloat() - rng.NextFloat()) * 0.4f);
    }

    void Entity::WaterSwimSound() {
        // MC waterSwimSound: the stroke's volume from the controlling
        // passenger's motion (0.4 for a ridden mount, 0.35 on its own).
        const Entity* controller = GetControllingPassenger();
        const Entity& mover = controller ? *controller : *this;
        const float modifier = controller ? 0.4f : 0.35f;
        const glm::dvec3 d = mover.velocity;
        const float speed = std::min(1.0f, static_cast<float>(std::sqrt(d.x * d.x * 0.2 + d.y * d.y + d.z * d.z * 0.2)) * modifier);
        PlaySwimSound(speed);
    }

    void Entity::DoWaterSplashEffect() {
        // MC doWaterSplashEffect, sound half (the BUBBLE / SPLASH particles
        // have no client particle type here): quieter strokes below 0.25 use
        // the plain splash, a hard entry the high-speed one.
        if (!m_level) return;
        const Entity* controller = GetControllingPassenger();
        const Entity& mover = controller ? *controller : *this;
        const float modifier = controller ? 0.9f : 0.2f;
        const glm::dvec3 d = mover.velocity;
        const float speed = std::min(1.0f, static_cast<float>(std::sqrt(d.x * d.x * 0.2 + d.y * d.y + d.z * d.z * 0.2)) * modifier);
        JavaRandom& rng = m_level->Random();
        const float pitch = 1.0f + (rng.NextFloat() - rng.NextFloat()) * 0.4f;
        PlaySound(speed < 0.25f ? GetSwimSplashSound() : GetSwimHighSpeedSplashSound(), speed, pitch);
    }

    void Entity::ApplyMovementEmissionAndPlaySound(const glm::dvec3& movement) {
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return;

        const float movedDistance = static_cast<float>(glm::length(movement) * 0.6000000238418579);
        const float horizontalMovedDistance = static_cast<float>(
            std::sqrt(movement.x * movement.x + movement.z * movement.z) * 0.6000000238418579);

        // MC getOnPos() (offset 1e-5) — the block the entity stands on — and
        // getOnPosLegacy() (offset 0.2), the "effect" block (a carpet on it).
        const int bx = static_cast<int>(std::floor(position.x));
        const int bz = static_cast<int>(std::floor(position.z));
        const glm::ivec3 supportingPos(bx, static_cast<int>(std::floor(position.y - 1.0e-5)), bz);
        const glm::ivec3 effectPos(bx, static_cast<int>(std::floor(position.y - 0.2)), bz);
        const BlockState supportingState = blocks->GetBlockState(supportingPos.x, supportingPos.y, supportingPos.z);
        const BlockState effectState = blocks->GetBlockState(effectPos.x, effectPos.y, effectPos.z);

        const bool climbing = IsClimbableForSound(supportingState.Block());
        m_moveDist += climbing ? movedDistance : horizontalMovedDistance;
        m_flyDist += movedDistance;

        const bool supportingIsAir = supportingState.Block() == BlockID::Air;
        if (m_moveDist > m_nextStep && !supportingIsAir) {
            LivingEntity* living = AsLiving();
            const bool swimming = living && living->IsSwimming();
            // MC vibrationAndSoundEffectsFromBlock: a step lands when the
            // entity is on the ground (or climbing) and not swimming.
            const auto stepOn = [&](const glm::ivec3& pos, BlockState state, bool shouldSound) {
                if (state.Block() == BlockID::Air) return false;
                const bool isClimbable = IsClimbableForSound(state.Block());
                if (!(onGround || isClimbable) || swimming) return false;
                if (shouldSound) {
                    // MC walkingStepSound: the step, and amethyst's chime.
                    PlayStepSound(pos, state);
                    if (IsCrystalSoundBlock(state.Block()) && tickCount >= m_lastCrystalSoundPlayTick + 20) {
                        // MC playAmethystStepSound.
                        m_crystalSoundIntensity *= static_cast<float>(
                            std::pow(0.997, static_cast<double>(tickCount - m_lastCrystalSoundPlayTick)));
                        m_crystalSoundIntensity = std::min(1.0f, m_crystalSoundIntensity + 0.07f);
                        JavaRandom& rng = m_level->Random();
                        const float pitch = 0.5f + m_crystalSoundIntensity * rng.NextFloat() * 1.2f;
                        const float volume = 0.1f + m_crystalSoundIntensity * 1.2f;
                        PlaySound(SoundEvents::AMETHYST_BLOCK_CHIME, volume, pitch);
                        m_lastCrystalSoundPlayTick = tickCount;
                    }
                }
                return true;
            };
            const bool onlyEffectState = supportingPos == effectPos;
            bool produced = stepOn(effectPos, effectState, true);
            if (!onlyEffectState) produced |= stepOn(supportingPos, supportingState, false);
            if (produced) {
                m_nextStep = NextStep();
            } else if (IsInWater()) {
                m_nextStep = NextStep();
                WaterSwimSound();
            }
        } else if (supportingIsAir) {
            // MC processFlappingMovement.
            if (IsFlapping()) OnFlap();
        }
    }

    void Entity::PlaySound(std::string_view event, float volume, float pitch) {
        // MC Entity.playSound: level.playSound(null, getX(), getY(), getZ(),
        // sound, getSoundSource(), volume, pitch) unless silent. A null
        // `except` means the server sends it to every nearby player and a
        // client-side copy of this entity stays quiet — the server's is the
        // one everyone hears.
        if (IsSilent() || !m_level || event.empty()) return;
        m_level->PlaySound(nullptr, position, event, GetSoundSource(), volume, pitch);
    }

    void Entity::Move(const glm::dvec3& delta) {
        PROFILE_ZONE_N("Entity.Move");
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

        // MC EntityCollisionContext: the Aether's aerclouds shape by the
        // mover's fall distance (AercloudBlock.hpp) — a mob falling faster
        // lands on the 0.9 falling shape, one drifting down sinks through
        // the thin floor, and the entityInside hooks (BlockBehaviors.cpp)
        // slow it. No mob glides here.
        PhysicsContext ctx = m_level->Physics();
        ctx.collisionEntity       = true;
        ctx.collisionFallDistance = fallDistance;

        // MoveEntity consumes the velocity vector it is handed, zeroing blocked
        // axes. MC passes deltaMovement itself, so the caller's velocity is the
        // thing that must be mutated — hand it the real member, not a copy.
        const double oldY = position.y;
        const glm::dvec3 startPos = position;
        glm::dvec3 vel = delta;
        // A move is a step, never a teleport. MoveEntity sweeps the box the
        // step covers and IsRegionAllAir walks every chunk column of it, so
        // a delta the size of the world (a velocity computed from a
        // position that just teleported 300,000 blocks) costs tens of
        // thousands of chunk lookups per collision test and freezes the
        // thread for seconds a tick — on the server, and again on every
        // client the velocity packet reaches. Nothing legitimate moves an
        // entity a hundred blocks in one tick (terminal velocity is under
        // four); anything that asks to is dropped and named in the log.
        {
            constexpr double kMaxStep = 64.0;
            const double stepSq = glm::dot(vel, vel);
            if (!(stepSq <= kMaxStep * kMaxStep)) {   // also catches NaN
                static std::atomic<int64_t> s_lastLogMs{0};
                const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (nowMs - s_lastLogMs.load(std::memory_order_relaxed) > 1000) {
                    s_lastLogMs.store(nowMs, std::memory_order_relaxed);
                    Log::Warning("[Entity] %s #%d at (%.1f, %.1f, %.1f) asked to move (%.1f, %.1f, %.1f) in one tick — dropped",
                                 std::string(TypeInfo().slug).c_str(), GetId(), position.x, position.y, position.z,
                                 vel.x, vel.y, vel.z);
                }
                velocity = glm::dvec3(0.0);
                return;
            }
        }
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

        // MC Entity.move → applyMovementEmissionAndPlaySound, server side
        // (`!level.isClientSide() || isLocalInstanceAuthoritative()` — a mob
        // mirror is never authoritative, and its sounds would be dropped by
        // the client bridge anyway). Passengers already returned above.
        if (!m_level->IsClientSide() && !IsRemoved() && EmitsMovementSounds()) {
            ApplyMovementEmissionAndPlaySound(position - startPos);
        }

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
                // MC Block.fallOn: causeFallDamage(fd * (1 - fallDistance
                // Reduction), 1.0F) for the block landed on (getOnPosLegacy
                // = getOnPos(0.2)) — a bed or shelf mushroom halves the
                // fall. (Hay's 0.2 multiplier and slime's cancel are their
                // own fallOn overrides, still to come.)
                //
                // Riders take the landing too — MC does this inside
                // causeFallDamage itself; see PropagateFallToPassengers's
                // header note for why it is called from here instead.
                float reduction = 0.0f;
                if (const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr) {
                    const glm::ivec3 p = BlockPosition();
                    reduction = FallDistanceReduction(blocks->GetBlock(
                        p.x, static_cast<int>(std::floor(position.y - 0.2)), p.z));
                }
                const float fd = fallDistance * (1.0f - reduction);
                PropagateFallToPassengers(fd, 1.0f);
                CauseFallDamage(fd, 1.0f);
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

    // MC EntityFluidInteraction.update, with the two per-fluid trackers and
    // current accumulators inlined as FluidContact.
    bool Entity::UpdateFluidInteraction(bool ignoreCurrent) {
        m_fluid = FluidContact{};
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;

        // MC getFluidInteractionBox: the bounding box deflated by 0.001 so a
        // box exactly flush with a fluid cell's face does not count as in it.
        const AABBd full = GetAABBd();
        const glm::dvec3 lo = full.min + 0.001;
        const glm::dvec3 hi = full.max - 0.001;
        const int x0 = static_cast<int>(std::floor(lo.x));
        const int y0 = static_cast<int>(std::floor(lo.y));
        const int z0 = static_cast<int>(std::floor(lo.z));
        const int x1 = static_cast<int>(std::ceil(hi.x)) - 1;
        const int y1 = static_cast<int>(std::ceil(hi.y)) - 1;
        const int z1 = static_cast<int>(std::ceil(hi.z)) - 1;

        // MC hasFluidAndLoaded: skip the cell walk when no section in range
        // holds a fluid. The engine's section-emptiness test is the same
        // early-out for the common case — an entity in open air.
        if (blocks->IsRegionAllAir(glm::ivec3(x0 - 1, y0, z0 - 1),
                                   glm::ivec3(x1 + 1, y1, z1 + 1))) {
            return false;
        }

        const double entityY  = full.min.y;
        const int    eyeBlockX = static_cast<int>(std::floor(position.x));
        const double eyeY      = GetEyeY();
        const int    eyeBlockZ = static_cast<int>(std::floor(position.z));
        bool any = false;

        for (int x = x0; x <= x1; ++x) {
            for (int y = y0; y <= y1; ++y) {
                for (int z = z0; z <= z1; ++z) {
                    const FluidState fluidState = GetFluidState(*blocks, x, y, z);
                    if (fluidState.IsEmpty()) continue;
                    const glm::ivec3 cell(x, y, z);
                    const double fluidBottom = static_cast<double>(y);
                    const double fluidTop = fluidBottom + FluidHeight(*blocks, cell, fluidState);
                    if (fluidTop < lo.y) continue;

                    any = true;
                    const int t = static_cast<int>(fluidState.type);
                    if (x == eyeBlockX && z == eyeBlockZ && eyeY >= fluidBottom) {
                        const double fluidTopForCamera =
                            fluidBottom + FluidHeightForCamera(*blocks, cell, fluidState);
                        if (eyeY <= fluidTopForCamera) m_fluid.eyesInside[t] = true;
                    }

                    m_fluid.height[t] = std::max(fluidTop - entityY, m_fluid.height[t]);
                    if (!ignoreCurrent) {
                        glm::dvec3 flow = FluidFlow(*blocks, cell, fluidState);
                        m_fluid.currentHeight[t] = std::max(m_fluid.height[t], m_fluid.currentHeight[t]);
                        // A shallow film pushes proportionally less.
                        if (m_fluid.currentHeight[t] < 0.4) flow *= m_fluid.currentHeight[t];
                        m_fluid.current[t] += flow;
                        ++m_fluid.currentCount[t];
                    }
                }
            }
        }
        return any;
    }

    // MC EntityFluidInteraction.CurrentAccumulator.applyTo.
    void Entity::ApplyFluidCurrent(FluidType type, double scale) {
        const int t = static_cast<int>(type);
        if (m_fluid.currentCount[t] == 0) return;
        const glm::dvec3 acc = m_fluid.current[t];
        if (glm::dot(acc, acc) < 9.999999747378752E-6) return;

        // A player averages the cells' flows; everything else takes the
        // unit direction (so a mob in a wide river is pushed at full
        // strength regardless of how many cells it overlaps).
        glm::dvec3 impulse = IsPlayer()
            ? acc * (1.0 / static_cast<double>(m_fluid.currentCount[t]))
            : glm::normalize(acc);
        impulse *= scale;

        // The minimum-nudge rule: something sitting still in a current is
        // always moved at least 0.0045 a tick, or a slow flow could never
        // overcome the rest.
        constexpr double kMin = 0.003;
        if (std::abs(velocity.x) < kMin && std::abs(velocity.z) < kMin &&
            glm::length(impulse) < 0.0045000000000000005) {
            impulse = glm::normalize(impulse) * 0.0045000000000000005;
        }
        AddDeltaMovement(impulse);
    }

    void Entity::LavaIgnite() {
        if (!FireImmune()) IgniteForSeconds(15);
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

        UpdateInWaterStateAndDoFluidPushing();

        // MC Entity.baseTick: `if (isOnFire() && (isInPowderSnow ||
        // isInWaterOrRain() || isInFloatableFluid())) clearFire()`. No rain
        // or powder snow here; water is the whole rule.
        if (IsOnFire() && IsInWater()) {
            ClearFire();
            // MC applyEffectsFromBlocks: `wasOnFire && !isOnFire()` → the hiss.
            PlayEntityOnFireExtinguishedSound();
        }
        if (m_remainingFireTicks > 0) {
            --m_remainingFireTicks;
        }
        // MC Entity.baseTick: lava only HALVES the fall each tick (a lava
        // landing still hurts); water's reset lives in the fluid update.
        if (IsInLava()) {
            fallDistance *= 0.5f;
        }

        // MC Entity.checkBelowWorld — anything that falls out of the world is
        // discarded rather than left falling forever. The threshold is the
        // world floor minus 64, matching MC.
        if (position.y < -64.0 - 64.0) {
            Discard();
        }
    }

    void Entity::UpdateInWaterStateAndDoFluidPushing() {
        // MC Entity.updateFluidInteraction.
        const bool pushed = IsPushedByFluid();
        UpdateFluidInteraction(/*ignoreCurrent=*/!pushed);
        const bool inWater = GetFluidHeight(FluidType::Water) > 0.0;
        const bool inLava  = GetFluidHeight(FluidType::Lava)  > 0.0;
        if (inWater) {
            ResetFallDistance();
            // MC doWaterSplashEffect on the dry→wet edge (`!wasTouchingWater
            // && !firstTick`): the splash sound, from the server. The SPLASH
            // / BUBBLE particles have no client particle type yet.
            if (!m_wasTouchingWater && m_fluidPrimed && m_level && !m_level->IsClientSide()) {
                DoWaterSplashEffect();
            }
        }
        m_wasTouchingWater = inWater;
        m_fluidPrimed = true;

        if (pushed) {
            if (inWater) ApplyFluidCurrent(FluidType::Water, 0.014);
            if (inLava) {
                // Lava runs slower: 0.007 where the dimension is FAST_LAVA
                // (the nether), 0.0023333… elsewhere.
                const bool fastLava = m_level && m_level->Dimension() == DimensionId::Nether;
                ApplyFluidCurrent(FluidType::Lava, fastLava ? 0.007 : 0.0023333333333333335);
            }
        }

        // LavaFluid.entityInside: ignite for 15 s, then hurt (4 damage,
        // gated by the hurt cooldown as in vanilla — a swim is ~2 hearts a
        // half-second). WaterFluid.entityInside's EXTINGUISH is the clearFire
        // in BaseTick. Both are applied from checkInsideBlocks in MC, i.e.
        // once per tick per overlapped fluid, which is what this is.
        if (inLava) {
            LavaIgnite();
            LavaHurt();
        }
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

