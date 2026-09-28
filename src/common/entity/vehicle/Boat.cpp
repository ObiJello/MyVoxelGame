// File: src/common/entity/vehicle/Boat.cpp
#include "common/entity/vehicle/Boat.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Leashable.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockFriction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <vector>

namespace Game {

    namespace {

        int FloorI(double v) { return static_cast<int>(std::floor(v)); }
        int CeilI(double v)  { return static_cast<int>(std::ceil(v)); }

        // MC Mth.clampedLerp(factor, min, max).
        float ClampedLerp(float factor, float min, float max) {
            if (factor < 0.0f) return min;
            if (factor > 1.0f) return max;
            return min + factor * (max - min);
        }

        // One entity-type tag as a per-type table, resolved once from the
        // data pack (DataTags) — the tag files are the truth.
        class EntityTypeTagTable {
        public:
            explicit EntityTypeTagTable(const char* tag) : m_tag(tag) {}
            bool Has(EntityTypeId type) {
                std::call_once(m_once, [this] {
                    m_bits.assign(static_cast<size_t>(kEntityTypeCount), 0);
                    for (int i = 0; i < kEntityTypeCount; ++i) {
                        const std::string slug(kEntityTypeTable[i].slug);
                        m_bits[static_cast<size_t>(i)] =
                            DataTags::HasTag(DataTags::Registry::EntityType, slug, m_tag) ? 1 : 0;
                    }
                });
                const size_t i = static_cast<size_t>(type);
                return i < m_bits.size() && m_bits[i] != 0;
            }
        private:
            const char*          m_tag;
            std::once_flag       m_once;
            std::vector<uint8_t> m_bits;
        };

        EntityTypeTagTable s_cannotBePushedOntoBoats("minecraft:cannot_be_pushed_onto_boats");
        EntityTypeTagTable s_canTurnInBoats("minecraft:can_turn_in_boats");

        bool IsWaterFluid(const FluidState& fs) { return fs.Is(FluidType::Water); }

    } // namespace

    // ── Construction ───────────────────────────────────────────────────────

    Boat::Boat(EntityTypeId type, EntityLevel* level)
        : VehicleEntity(type, level) {
        if (IsChestBoatEntityType(type)) {
            m_container = std::make_unique<VehicleContainer>(*this, kChestSize);
        }
    }

    Boat::~Boat() = default;

    void Boat::SetInitialPos(const glm::dvec3& pos) {
        position = pos;
        oldPosition = pos;
    }

    // ── Seats ──────────────────────────────────────────────────────────────

    double Boat::RideHeight() const {
        // Boat: dimensions.height() / 3; Raft: height * 0.8888889.
        const double h = static_cast<double>(GetBbHeight());
        return IsRaft() ? static_cast<double>(static_cast<float>(h) * 0.8888889f)
                        : static_cast<double>(static_cast<float>(h) / 3.0f);
    }

    glm::dvec3 Boat::PassengerAttachmentAt(int slot, int total, bool animalPassenger, float vehicleYRot) const {
        // MC AbstractBoat.getPassengerAttachmentPoint.
        float offset = IsChest() ? 0.15f : 0.0f;   // getSinglePassengerXOffset
        if (total > 1) {
            offset = slot == 0 ? 0.2f : -0.6f;
            if (animalPassenger) offset += 0.2f;
        }
        // new Vec3(0, rideHeight, offset).yRot(-yRot * DEG).
        const double a = -static_cast<double>(vehicleYRot) * static_cast<double>(Mth::kDegToRad);
        const double c = std::cos(a), s = std::sin(a);
        const double z = static_cast<double>(offset);
        return glm::dvec3(z * s, RideHeight(), z * c);
    }

    bool Boat::CanAddPassenger(const Entity& passenger) const {
        (void)passenger;
        // MC: passengers < max && !isEyeInFluid(WATER).
        return static_cast<int>(GetPassengers().size()) < GetMaxPassengers() && !IsEyeInWater();
    }

    float Boat::PassengerBodyYRot(bool animal, int32_t passengerId) const {
        // MC calculatePassengerBodyYRot: an animal in a full boat faces
        // sideways, by its id's parity.
        if (animal && PassengerTotal() == GetMaxPassengers()) {
            return yRot + (passengerId % 2 == 0 ? 90.0f : 270.0f);
        }
        return yRot;
    }

    float Boat::ClampPassengerYaw(float passengerYRot, float boatBodyYRot) {
        // MC clampRotation's non-mob branch.
        const float delta = Mth::WrapDegrees(passengerYRot - boatBodyYRot);
        const float targetDelta = std::clamp(delta, -105.0f, 105.0f);
        return targetDelta - delta;
    }

    void Boat::PositionRider(Entity& passenger) {
        // MC AbstractBoat.positionRider: super, then (unless the rider may
        // turn in boats) carry an authoritative rider round with the boat
        // and clamp its facing.
        VehicleEntity::PositionRider(passenger);
        if (!HasPassenger(passenger)) return;
        if (s_canTurnInBoats.Has(passenger.GetType()) && !passenger.IsPlayer()) return;
        auto* living = passenger.AsLiving();
        const bool isMob = dynamic_cast<Mob*>(&passenger) != nullptr && !passenger.IsPlayer();
        // passenger.isLocalInstanceAuthoritative(): a mob on the server;
        // never a player's view (the player's client turns its own view).
        const bool authoritative = isMob && m_level && !m_level->IsClientSide();
        if (authoritative) {
            passenger.yRot += m_deltaRotation;
            if (living) living->yHeadRot += m_deltaRotation;
        }
        // clampRotation(passenger).
        const bool animal = dynamic_cast<const Animal*>(&passenger) != nullptr;
        const float bodyYRot = PassengerBodyYRot(animal, passenger.GetId());
        if (living) living->yBodyRot = bodyYRot;
        if (isMob) {
            passenger.yRot = bodyYRot;
            // Mob.clampHeadRotationToBody: the head within its max turn of
            // the body.
            if (living) {
                const float maxHead = static_cast<float>(living->GetMaxHeadYRot());
                const float headDelta = Mth::WrapDegrees(living->yHeadRot - bodyYRot);
                const float clamped = std::clamp(headDelta, -maxHead, maxHead);
                living->yHeadRot = bodyYRot + clamped;
            }
        } else if (!passenger.IsPlayer()) {
            passenger.yRot += ClampPassengerYaw(passenger.yRot, bodyYRot);
        }
    }

    glm::dvec3 Boat::GetDismountLocationForPassenger(const LivingEntity& passenger) const {
        // MC AbstractBoat.getDismountLocationForPassenger.
        const double passengerWidth = PassengerWidth(passenger);
        const glm::dvec3 direction = CollisionHorizontalEscapeVector(
            static_cast<double>(GetBbWidth()) * 1.4142135623730951, passengerWidth, passenger.yRot);
        const double targetX = position.x + direction.x;
        const double targetZ = position.z + direction.z;
        const AABBd box = GetAABBd();
        const glm::ivec3 targetBlockPos(FloorI(targetX), FloorI(box.max.y), FloorI(targetZ));
        const glm::ivec3 belowBlockPos(targetBlockPos.x, targetBlockPos.y - 1, targetBlockPos.z);
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (blocks && !IsWaterFluid(GetFluidState(*blocks, belowBlockPos))) {
            std::vector<glm::dvec3> targets;
            const double targetFloor = BlockFloorHeight(*blocks, targetBlockPos);
            if (IsBlockFloorValid(targetFloor)) {
                targets.emplace_back(targetX, static_cast<double>(targetBlockPos.y) + targetFloor, targetZ);
            }
            const double belowFloor = BlockFloorHeight(*blocks, belowBlockPos);
            if (IsBlockFloorValid(belowFloor)) {
                targets.emplace_back(targetX, static_cast<double>(belowBlockPos.y) + belowFloor, targetZ);
            }
            std::vector<double> heights;
            DismountPoseHeights(passenger, heights);
            const double hw = passengerWidth * 0.5;
            for (double h : heights) {
                for (const glm::dvec3& t : targets) {
                    const AABBd body = AABBd::FromMinMax(glm::dvec3(t.x - hw, t.y, t.z - hw),
                                                         glm::dvec3(t.x + hw, t.y + h, t.z + hw));
                    if (CanDismountTo(*blocks, body)) return t;
                }
            }
        }
        return VehicleEntity::GetDismountLocationForPassenger(passenger);
    }

    // ── Status ─────────────────────────────────────────────────────────────

    Boat::Status Boat::ComputeStatus() {
        // MC getStatus.
        if (const std::optional<Status> water = IsUnderwaterStatus()) {
            m_waterLevel = GetAABBd().max.y;
            return *water;
        }
        if (CheckInWater()) return Status::InWater;
        const float friction = GetGroundFriction();
        if (friction > 0.0f) {
            m_landFriction = friction;
            return Status::OnLand;
        }
        return Status::InAir;
    }

    float Boat::GetWaterLevelAbove() const {
        // MC getWaterLevelAbove.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        const AABBd aabb = GetAABBd();
        const int minX = FloorI(aabb.min.x);
        const int maxX = CeilI(aabb.max.x);
        const int minY = FloorI(aabb.max.y);
        const int maxY = CeilI(aabb.max.y - m_lastYd);
        const int minZ = FloorI(aabb.min.z);
        const int maxZ = CeilI(aabb.max.z);
        if (!blocks) return static_cast<float>(maxY + 1);
        int y = minY;
        for (; y < maxY; ++y) {
            float blockHeight = 0.0f;
            bool full = false;
            for (int x = minX; x < maxX && !full; ++x) {
                for (int z = minZ; z < maxZ; ++z) {
                    const glm::ivec3 pos(x, y, z);
                    const FluidState fs = GetFluidState(*blocks, pos);
                    if (IsWaterFluid(fs)) blockHeight = std::max(blockHeight, FluidHeight(*blocks, pos, fs));
                    if (blockHeight >= 1.0f) { full = true; break; }
                }
            }
            if (full) continue;   // `continue label39`
            if (blockHeight < 1.0f) {
                // MC returns pos.getY() + blockHeight, where pos was last set
                // in the loop — the row y.
                return static_cast<float>(y) + blockHeight;
            }
        }
        return static_cast<float>(maxY + 1);
    }

    float Boat::GetGroundFriction() const {
        // MC getGroundFriction.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 0.0f;
        const AABBd bb = GetAABBd();
        const AABBd box = AABBd::FromMinMax(glm::dvec3(bb.min.x, bb.min.y - 0.001, bb.min.z),
                                            glm::dvec3(bb.max.x, bb.min.y, bb.max.z));
        const int x0 = FloorI(box.min.x) - 1;
        const int x1 = CeilI(box.max.x) + 1;
        const int y0 = FloorI(box.min.y) - 1;
        const int y1 = CeilI(box.max.y) + 1;
        const int z0 = FloorI(box.min.z) - 1;
        const int z1 = CeilI(box.max.z) + 1;
        float friction = 0.0f;
        int count = 0;
        for (int x = x0; x < x1; ++x) {
            for (int z = z0; z < z1; ++z) {
                const int edges = ((x != x0 && x != x1 - 1) ? 0 : 1) + ((z != z0 && z != z1 - 1) ? 0 : 1);
                if (edges == 2) continue;
                for (int y = y0; y < y1; ++y) {
                    if (!(edges <= 0 || (y != y0 && y != y1 - 1))) continue;
                    const BlockState state = blocks->GetBlockState(x, y, z);
                    const BlockID id = state.Block();
                    if (id == BlockID::LilyPad || !BlockRegistry::HasCollision(id)) continue;
                    // Shapes.joinIsNotEmpty(shape.move(pos), boatShape, AND):
                    // some collision box shares volume with the thin slab.
                    const BlockRegistry::BlockShapeSet shape = BlockRegistry::GetBlockCollisionShapeSet(state);
                    bool touches = false;
                    for (const auto& b : shape) {
                        const glm::dvec3 lo = glm::dvec3(x, y, z) + glm::dvec3(b.min);
                        const glm::dvec3 hi = glm::dvec3(x, y, z) + glm::dvec3(b.max);
                        if (lo.x < box.max.x && hi.x > box.min.x && lo.y < box.max.y && hi.y > box.min.y &&
                            lo.z < box.max.z && hi.z > box.min.z) {
                            touches = true;
                            break;
                        }
                    }
                    if (!touches) continue;
                    friction += GetBlockFriction(id);
                    ++count;
                }
            }
        }
        // friction / count — a NaN for no support in MC, which reads as "not
        // on land"; 0 here.
        return count > 0 ? friction / static_cast<float>(count) : 0.0f;
    }

    bool Boat::CheckInWater() {
        // MC checkInWater.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        const AABBd bb = GetAABBd();
        const int minX = FloorI(bb.min.x);
        const int maxX = CeilI(bb.max.x);
        const int minY = FloorI(bb.min.y);
        const int maxY = CeilI(bb.min.y + 0.001);
        const int minZ = FloorI(bb.min.z);
        const int maxZ = CeilI(bb.max.z);
        bool inWater = false;
        m_waterLevel = -1.7976931348623157E308;
        if (!blocks) return false;
        for (int x = minX; x < maxX; ++x) {
            for (int y = minY; y < maxY; ++y) {
                for (int z = minZ; z < maxZ; ++z) {
                    const glm::ivec3 pos(x, y, z);
                    const FluidState fs = GetFluidState(*blocks, pos);
                    if (!IsWaterFluid(fs)) continue;
                    const float height = static_cast<float>(y) + FluidHeight(*blocks, pos, fs);
                    m_waterLevel = std::max(static_cast<double>(height), m_waterLevel);
                    inWater |= bb.min.y < static_cast<double>(height);
                }
            }
        }
        return inWater;
    }

    std::optional<Boat::Status> Boat::IsUnderwaterStatus() {
        // MC isUnderwater.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return std::nullopt;
        const AABBd aabb = GetAABBd();
        const double maxY = aabb.max.y + 0.001;
        const int x0 = FloorI(aabb.min.x);
        const int x1 = CeilI(aabb.max.x);
        const int y0 = FloorI(aabb.max.y);
        const int y1 = CeilI(maxY);
        const int z0 = FloorI(aabb.min.z);
        const int z1 = CeilI(aabb.max.z);
        bool underWater = false;
        for (int x = x0; x < x1; ++x) {
            for (int y = y0; y < y1; ++y) {
                for (int z = z0; z < z1; ++z) {
                    const glm::ivec3 pos(x, y, z);
                    const FluidState fs = GetFluidState(*blocks, pos);
                    if (IsWaterFluid(fs) &&
                        maxY < static_cast<double>(static_cast<float>(y) + FluidHeight(*blocks, pos, fs))) {
                        if (!fs.IsSource()) return Status::UnderFlowingWater;
                        underWater = true;
                    }
                }
            }
        }
        if (underWater) return Status::UnderWater;
        return std::nullopt;
    }

    // ── Physics ────────────────────────────────────────────────────────────

    void Boat::FloatBoat() {
        // MC floatBoat.
        double vspeed = -GetGravity();
        double buoyancy = 0.0;
        float invFriction = 0.05f;
        if (m_oldStatus == Status::InAir && m_status != Status::InAir && m_status != Status::OnLand) {
            m_waterLevel = position.y + static_cast<double>(GetBbHeight());   // getY(1.0)
            const double targetY = static_cast<double>(GetWaterLevelAbove() - GetBbHeight()) + 0.101;
            AABBd moved = GetAABBd();
            moved.min.y += targetY - position.y;
            moved.max.y += targetY - position.y;
            if (NoCollision(moved)) {
                position.y = targetY;
                velocity.y = 0.0;   // multiply(1, 0, 1)
                m_lastYd = 0.0;
            }
            m_status = Status::InWater;
        } else {
            if (m_status == Status::InWater) {
                buoyancy = (m_waterLevel - position.y) / static_cast<double>(GetBbHeight());
                invFriction = 0.9f;
            } else if (m_status == Status::UnderFlowingWater) {
                vspeed = -7.0E-4;
                invFriction = 0.9f;
            } else if (m_status == Status::UnderWater) {
                buoyancy = 0.009999999776482582;
                invFriction = 0.45f;
            } else if (m_status == Status::InAir) {
                invFriction = 0.9f;
            } else if (m_status == Status::OnLand) {
                invFriction = m_landFriction;
                // `if (getControllingPassenger() instanceof Player)
                // landFriction /= 2` — the NEXT tick's friction if the
                // status is recomputed as on-land again (it always is: the
                // halving is undone by getStatus's reassignment).
                if (IsControlledByPlayer()) m_landFriction /= 2.0f;
            }
            velocity.x *= static_cast<double>(invFriction);
            velocity.y += vspeed;
            velocity.z *= static_cast<double>(invFriction);
            m_deltaRotation *= invFriction;
            if (buoyancy > 0.0) {
                velocity.y = (velocity.y + buoyancy * (0.04 / 0.65)) * 0.75;   // getDefaultGravity() / 0.65
            }
        }
    }

    void Boat::ControlBoat() {
        // MC controlBoat (the controlling client only).
        if (!IsVehicle() && PassengerTotal() == 0) return;   // isVehicle
        float acceleration = 0.0f;
        if (m_inputLeft)  m_deltaRotation -= 1.0f;
        if (m_inputRight) m_deltaRotation += 1.0f;
        if (m_inputRight != m_inputLeft && !m_inputUp && !m_inputDown) acceleration += 0.005f;
        yRot += m_deltaRotation;
        if (m_inputUp)   acceleration += 0.04f;
        if (m_inputDown) acceleration -= 0.005f;
        // Mth.sin(-yRot * DEG) * a, Mth.cos(yRot * DEG) * a.
        velocity.x += static_cast<double>(std::sin(-yRot * Mth::kDegToRad) * acceleration);
        velocity.z += static_cast<double>(std::cos(yRot * Mth::kDegToRad) * acceleration);
        // MC's paddle state, plus (a deliberate addition) both paddles rowing
        // in reverse while only backward is held — the boat already moves
        // back in MC (the -0.005), it just looked like it drifted.
        const bool backPaddle = m_inputDown && !m_inputUp;
        SetPaddleState((m_inputRight && !m_inputLeft) || m_inputUp || backPaddle,
                       (m_inputLeft && !m_inputRight) || m_inputUp || backPaddle, backPaddle);
    }

    void Boat::VehicleCheckFallDamage(double ya, bool onGroundNow) {
        // MC AbstractBoat.checkFallDamage — a boat never hurts itself or its
        // riders landing; it only keeps the fall for the float's re-seat.
        m_lastYd = velocity.y;
        if (IsPassenger()) return;
        if (onGroundNow) {
            ResetFallDistance();
            return;
        }
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        const glm::ivec3 below(FloorI(position.x), FloorI(position.y) - 1, FloorI(position.z));
        const bool waterBelow = blocks && IsWaterFluid(GetFluidState(*blocks, below));
        if (!waterBelow && ya < 0.0) fallDistance -= static_cast<float>(ya);
    }

    // ── Tick ───────────────────────────────────────────────────────────────

    void Boat::BaseTick() {
        VehicleBaseTick();
    }

    void Boat::Tick() {
        // MC AbstractBoat.tick.
        const bool clientSide = !m_level || m_level->IsClientSide();
        m_oldStatus = m_status;
        m_status = ComputeStatus();
        if (m_status != Status::UnderWater && m_status != Status::UnderFlowingWater) {
            m_outOfControlTicks = 0.0f;
        } else {
            m_outOfControlTicks += 1.0f;
        }
        if (!clientSide && m_outOfControlTicks >= static_cast<float>(kTimeToEject)) {
            EjectPassengers();
        }
        TickHurtAndDamage();

        // super.tick() → Entity.tick → baseTick.
        BaseTick();
        if (IsRemoved()) return;

        if (IsLocalInstanceAuthoritative()) {
            if (!FirstPassengerIsPlayer()) SetPaddleState(false, false);
            FloatBoat();
            if (clientSide) {
                ControlBoat();
                // (ServerboundPaddleBoatPacket goes out with the move —
                // client/entity/ClientVehicles.)
            }
            // move(SELF, deltaMovement), with the boat's own fall rule.
            MoveVehicle(velocity);
        } else {
            velocity = glm::dvec3(0.0);
            m_deltaRotation = 0.0f;
        }

        // applyEffectsFromBlocks ×2: the bubble column under the hull (the
        // other block effects — lily pads, portals — run from the server's
        // per-tick inside-block pass).
        ApplyBubbleColumns();
        TickBubbleColumn();

        for (int i = 0; i <= 1; ++i) {
            if (GetPaddleState(i)) {
                // Reverse rowing runs the stroke backward; the stroke sound
                // lands on the same point of the (wrapped) cycle.
                const float step = m_paddleReverse ? -kPaddleSpeed : kPaddleSpeed;
                const auto wrap = [](float a) {
                    float r = std::fmod(a, 6.2831855f);
                    return r < 0.0f ? r + 6.2831855f : r;
                };
                const float phase = m_paddleReverse ? wrap(m_paddlePositions[static_cast<size_t>(i)] + step)
                                                    : wrap(m_paddlePositions[static_cast<size_t>(i)]);
                const float next = m_paddleReverse ? wrap(m_paddlePositions[static_cast<size_t>(i)])
                                                   : wrap(m_paddlePositions[static_cast<size_t>(i)] + step);
                if (!IsSilent() && static_cast<double>(phase) <= kPaddleSoundTime &&
                    static_cast<double>(next) >= kPaddleSoundTime) {
                    // getPaddleSound: water while floating (or under), land
                    // on land, none in the air.
                    const char* sound = nullptr;
                    switch (ComputeStatus()) {
                        case Status::InWater:
                        case Status::UnderWater:
                        case Status::UnderFlowingWater: sound = SoundEvents::BOAT_PADDLE_WATER; break;
                        case Status::OnLand:            sound = SoundEvents::BOAT_PADDLE_LAND;  break;
                        default: break;
                    }
                    if (sound && m_level && !clientSide) {
                        const glm::vec3 view = Mth::ViewVector(xRot, yRot);   // getViewVector(1)
                        const double dx = i == 1 ? -static_cast<double>(view.z) : static_cast<double>(view.z);
                        const double dz = i == 1 ? static_cast<double>(view.x) : -static_cast<double>(view.x);
                        JavaRandom& rng = m_level->Random();
                        m_level->PlaySound(nullptr, glm::dvec3(position.x + dx, position.y, position.z + dz), sound,
                                           GetSoundSource(), 1.0f, 0.8f + 0.4f * rng.NextFloat());
                    }
                }
                m_paddlePositions[static_cast<size_t>(i)] += step;
            } else {
                m_paddlePositions[static_cast<size_t>(i)] = 0.0f;
            }
        }

        PushAndPullEntities();
    }

    void Boat::PushAndPullEntities() {
        // MC AbstractBoat.tick's tail: entities within 0.2 of the hull (and
        // not above it) are pushed — or, on the server with nobody driving,
        // a living thing that fits is pulled aboard.
        if (!m_level) return;
        const bool clientSide = m_level->IsClientSide();
        const AABBd bb = GetAABBd();
        const AABB query = AABB::FromMinMax(glm::vec3(bb.min + glm::dvec3(-0.20000000298023224, 0.009999999776482582, -0.20000000298023224)),
                                            glm::vec3(bb.max + glm::dvec3(0.20000000298023224, -0.009999999776482582, 0.20000000298023224)));
        std::vector<Entity*> entities;
        m_level->GetEntitiesInBox(query, this, entities);
        // EntitySelector.pushableBy(this): pushable; on a client only the
        // local player, who is not an entity here.
        std::erase_if(entities, [&](Entity* e) {
            return !e || !e->IsPushable() || e->IsSpectator() || clientSide;
        });
        if (entities.empty()) return;
        const bool addNewPassengers = !clientSide && !IsControlledByPlayer();
        for (Entity* entity : entities) {
            if (entity->HasPassenger(*this)) continue;
            if (addNewPassengers && static_cast<int>(GetPassengers().size()) < GetMaxPassengers() &&
                !entity->IsPassenger() && HasEnoughSpaceFor(*entity) && entity->AsLiving() &&
                !entity->IsPlayer() && !s_cannotBePushedOntoBoats.Has(entity->GetType()) &&
                !IsVehicleEntityType(entity->GetType())) {
                if (entity->StartRiding(*this)) {
                    entity->GameEvent(GameEventId::EntityMount, entity);
                }
            } else {
                PushEntity(*entity);
            }
        }
    }

    void Boat::PushEntity(Entity& other) {
        // MC AbstractBoat.push(Entity).
        const AABBd mine = GetAABBd();
        const AABBd theirs = other.GetAABBd();
        if (IsBoatEntityType(other.GetType())) {
            if (theirs.min.y < mine.max.y) VehicleEntity::PushEntity(other);
        } else if (theirs.min.y <= mine.min.y) {
            VehicleEntity::PushEntity(other);
        }
    }

    // ── Bubble columns ─────────────────────────────────────────────────────

    void Boat::ApplyBubbleColumns() {
        // BubbleColumnBlock.entityInside for the cells the hull overlaps:
        // with open air above, onAboveBubbleColumn; otherwise the entity's
        // onInsideBubbleColumn (Entity's default — a boat does not override
        // it).
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return;
        const AABBd bb = GetAABBd();
        constexpr double kShrink = 1.0e-5;
        const int x0 = FloorI(bb.min.x + kShrink), x1 = FloorI(bb.max.x - kShrink);
        const int y0 = FloorI(bb.min.y + kShrink), y1 = FloorI(bb.max.y - kShrink);
        const int z0 = FloorI(bb.min.z + kShrink), z1 = FloorI(bb.max.z - kShrink);
        for (int x = x0; x <= x1; ++x) {
            for (int y = y0; y <= y1; ++y) {
                for (int z = z0; z <= z1; ++z) {
                    const BlockState state = blocks->GetBlockState(x, y, z);
                    if (state.Block() != BlockID::BubbleColumn) continue;
                    const bool dragDown = BoolOf(state, PropertyId::DRAG);
                    const BlockState above = blocks->GetBlockState(x, y + 1, z);
                    const bool emptyAbove = !BlockRegistry::HasCollision(above.Block()) &&
                                            FluidStateOf(above).IsEmpty();
                    if (emptyAbove) {
                        OnAboveBubbleColumn(dragDown, glm::ivec3(x, y, z));
                    } else if (IsLocalInstanceAuthoritative()) {
                        // Entity.onInsideBubbleColumn.
                        if (dragDown) velocity.y = std::max(-0.3, velocity.y - 0.03);
                        else          velocity.y = std::min(0.7, velocity.y + 0.06);
                        ResetFallDistance();
                    }
                    return;   // one column is enough; MC visits the cell once
                }
            }
        }
    }

    void Boat::OnAboveBubbleColumn(bool dragDown, const glm::ivec3& pos) {
        (void)pos;
        // MC AbstractBoat.onAboveBubbleColumn.
        if (m_level && !m_level->IsClientSide()) {
            m_isAboveBubbleColumn = true;
            m_bubbleColumnDirectionIsDown = dragDown;
            if (m_bubbleTime == 0) m_bubbleTime = kBubbleTime;
        }
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        if (!IsBoatUnderWater() && rng.NextInt(100) == 0) {
            m_level->PlayLocalSound(position, GetSwimSplashSound(), GetSoundSource(), 1.0f,
                                    0.8f + 0.4f * rng.NextFloat(), false);
            m_level->AddParticle(ParticleOptions(ParticleKind::Splash), position.x + rng.NextFloat(),
                                 position.y + 0.7, position.z + rng.NextFloat(), 0.0, 0.0, 0.0);
            GameEvent(GameEventId::Splash, GetControllingPassenger());
        }
    }

    void Boat::TickBubbleColumn() {
        // MC tickBubbleColumn.
        if (!m_level) return;
        if (m_level->IsClientSide()) {
            if (m_bubbleTime > 0) m_bubbleMultiplier += 0.05f;
            else                  m_bubbleMultiplier -= 0.1f;
            m_bubbleMultiplier = std::clamp(m_bubbleMultiplier, 0.0f, 1.0f);
            m_bubbleAngleO = m_bubbleAngle;
            m_bubbleAngle = 10.0f * static_cast<float>(std::sin(0.5 * static_cast<double>(tickCount))) *
                            m_bubbleMultiplier;
            return;
        }
        if (!m_isAboveBubbleColumn) m_bubbleTime = 0;
        int bubbleTime = m_bubbleTime;
        if (bubbleTime > 0) {
            --bubbleTime;
            m_bubbleTime = bubbleTime;
            const int diff = kBubbleTime - bubbleTime - 1;
            if (diff > 0 && bubbleTime == 0) {
                m_bubbleTime = 0;
                if (m_bubbleColumnDirectionIsDown) {
                    EjectPassengers();
                    m_level->BroadcastEntityEvent(*this, 71);
                } else {
                    m_level->BroadcastEntityEvent(*this, 72);
                }
                HandleBubbleColumnEffect(m_bubbleColumnDirectionIsDown);
            }
            m_isAboveBubbleColumn = false;
        }
    }

    void Boat::HandleBubbleColumnEffect(bool dragDown) {
        // MC handleBubbleColumnEffect: `if (canSimulateMovement())`.
        if (!IsLocalInstanceAuthoritative()) return;
        if (dragDown) velocity.y -= 0.7;
        else          velocity.y = 0.6;
        needsSync = true;
    }

    void Boat::HandleEntityEvent(uint8_t id) {
        if (id == 71) { HandleBubbleColumnEffect(true); return; }
        if (id == 72) { HandleBubbleColumnEffect(false); return; }
        VehicleEntity::HandleEntityEvent(id);
    }

    float Boat::GetBubbleAngle(float a) const {
        return m_bubbleAngleO + a * (m_bubbleAngle - m_bubbleAngleO);
    }

    // ── Paddles ────────────────────────────────────────────────────────────

    bool Boat::GetPaddleState(int side) const {
        // MC: the synched flag && getControllingPassenger() != null.
        const bool flag = side == 0 ? m_paddleLeft : m_paddleRight;
        return flag && IsControlledByPlayer();
    }

    float Boat::GetRowingTime(int side, float a) const {
        // MC getRowingTime: clampedLerp(a, position - PADDLE_SPEED, position).
        if (!GetPaddleState(side)) return 0.0f;
        const float p = m_paddlePositions[static_cast<size_t>(side)];
        // Back-paddling runs the stroke the other way.
        return ClampedLerp(a, m_paddleReverse ? p + kPaddleSpeed : p - kPaddleSpeed, p);
    }

    // ── Breaking ───────────────────────────────────────────────────────────

    void Boat::Destroy(MobDamageSource source, Entity* attacker) {
        // AbstractBoat: destroy(level, getDropItem()); AbstractChestBoat adds
        // chestVehicleDestroyed.
        VehicleEntity::Destroy(source, attacker);
        if (m_container) m_container->ChestVehicleDestroyed();
    }

    // ── Interaction ────────────────────────────────────────────────────────

    UseResult Boat::VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) {
        (void)held;
        // AbstractBoat.interact: aboard unless sneaking or out of control;
        // the client answers SUCCESS without trying.
        UseResult result = UseResult::Pass;
        if (!sneaking && m_outOfControlTicks < 60.0f) {
            if (!m_level || m_level->IsClientSide()) {
                result = UseResult::Success;
            } else if (m_level->StartPlayerRiding(player, *this)) {
                result = UseResult::SuccessServer;
            }
        }
        if (result != UseResult::Pass || !m_container) return result;
        // AbstractChestBoat.interact: room aboard and no sneak → PASS;
        // otherwise the chest (interactWithContainerVehicle) and
        // CONTAINER_OPEN.
        if (CanAddPassenger(player) && !sneaking) return UseResult::Pass;
        if (!m_level || m_level->IsClientSide()) return UseResult::Success;
        if (m_level->OpenContainerEntityMenu(player, *this)) {
            GameEvent(GameEventId::ContainerOpen, &player);
        }
        return UseResult::Success;
    }

    void Boat::OnRemoving(RemovalReason reason) {
        if (!m_level || m_level->IsClientSide()) return;
        // RemovalReason.shouldDestroy: KILLED and DISCARDED.
        if (reason != RemovalReason::Killed && reason != RemovalReason::Discarded) return;
        // AbstractChestBoat.remove runs first (its super is AbstractBoat's).
        if (m_container) m_container->DropContents();
        if (IsLeashed()) DropLeash();
    }

    void Boat::StopOpenContainer(LivingEntity& player) {
        if (!m_container || !m_level) return;
        if (ILevelWrite* world = m_level->MutableBlocks()) {
            world->GameEvent(&player, GameEventId::ContainerClose, position);
        }
    }

    // ── Leads ──────────────────────────────────────────────────────────────

    std::array<glm::dvec3, 4> Boat::GetQuadLeashOffsets() const {
        // Leashable.createQuadLeashOffsets(this, 0.0, 0.64, 0.382, 0.88).
        return Leash::CreateQuadLeashOffsets(*this, 0.0, 0.64, 0.382, 0.88);
    }

    glm::dvec3 Boat::GetLeashOffset(float partialTicks) const {
        (void)partialTicks;
        // MC AbstractBoat.getLeashOffset: (0, 0.88 × height, 0.64 × width).
        return glm::dvec3(0.0, static_cast<double>(0.88f * GetBbHeight()), static_cast<double>(0.64f * GetBbWidth()));
    }

    void Boat::LeashTooFarBehaviour() {
        // Leashable.leashTooFarBehaviour: the lead drops.
        DropLeash();
    }

    // ── Wire ───────────────────────────────────────────────────────────────

    void Boat::FillSyncedData(VehicleSyncedData& out) const {
        VehicleEntity::FillSyncedData(out);
        out.paddleLeft  = m_paddleLeft;
        out.paddleRight = m_paddleRight;
        out.paddleReverse = m_paddleReverse;
        out.bubbleTime  = m_bubbleTime;
    }

    void Boat::ApplySyncedData(const VehicleSyncedData& in) {
        VehicleEntity::ApplySyncedData(in);
        // A boat this client drives keeps its own paddles (it is the source
        // of them); everyone else's follow the server.
        if (!m_localControlled) {
            m_paddleLeft  = in.paddleLeft;
            m_paddleRight = in.paddleRight;
            m_paddleReverse = in.paddleReverse;
        }
        m_bubbleTime = in.bubbleTime;
    }

} // namespace Game
