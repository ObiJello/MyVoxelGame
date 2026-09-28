// File: src/common/entity/vehicle/Minecart.cpp
#include "common/entity/vehicle/Minecart.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/Rails.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/HopperBlockEntity.hpp"
#include "common/world/block/entity/SpawnerBlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <vector>

namespace Game {

    namespace {

        int FloorI(double v) { return static_cast<int>(std::floor(v)); }

        // MC AbstractMinecart.EXITS: the two cells a rail shape joins,
        // relative to its own (a slope's low end one below).
        struct RailExits { glm::ivec3 e0, e1; };
        RailExits ExitsOf(RailShape shape) {
            const glm::ivec3 xNeg(-1, 0, 0), xPos(1, 0, 0), zNeg(0, 0, -1), zPos(0, 0, 1);
            const glm::ivec3 below(0, -1, 0);
            switch (shape) {
                case RailShape::NorthSouth:     return {zNeg, zPos};
                case RailShape::EastWest:       return {xNeg, xPos};
                case RailShape::AscendingEast:  return {xNeg + below, xPos};
                case RailShape::AscendingWest:  return {xNeg, xPos + below};
                case RailShape::AscendingNorth: return {zNeg, zPos + below};
                case RailShape::AscendingSouth: return {zNeg + below, zPos};
                case RailShape::SouthEast:      return {zPos, xPos};
                case RailShape::SouthWest:      return {zPos, xNeg};
                case RailShape::NorthWest:      return {zNeg, xNeg};
                case RailShape::NorthEast:      return {zNeg, xPos};
            }
            return {zNeg, zPos};
        }

        // #minecraft:rails.
        bool IsRailAt(const IBlockAccess& level, int x, int y, int z) {
            return IsRailBlock(level.GetBlock(x, y, z));
        }

        double HorizontalLength(const glm::dvec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }
        double HorizontalLengthSqr(const glm::dvec3& v) { return v.x * v.x + v.z * v.z; }

        // MC Entity.getDirection: the horizontal direction of yRot.
        Direction DirectionFromYRot(float yRot) {
            const int i = static_cast<int>(std::floor(static_cast<double>(yRot) / 90.0 + 0.5)) & 3;
            // Direction.from2DDataValue: south, west, north, east.
            static constexpr Direction kByData[4] = {Direction::South, Direction::West, Direction::North,
                                                     Direction::East};
            return kByData[i];
        }
        // #minecraft:furnace_minecart_fuel — coal and charcoal in 26.3.
        bool IsFurnaceMinecartFuel(ItemID item) {
            // Resolved per item on first ask and remembered (the tag file
            // is the truth; a lookup is a lock and a string compare).
            static std::mutex mutex;
            static std::unordered_map<ItemID, bool> cache;
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = cache.find(item);
            if (it != cache.end()) return it->second;
            const std::string_view slug = ItemRegistry::Slug(item);
            const bool fuel = !slug.empty() &&
                              DataTags::HasTag(DataTags::Registry::Item, slug, "minecraft:furnace_minecart_fuel");
            cache.emplace(item, fuel);
            return fuel;
        }

    } // namespace

    // ── AbstractMinecart ───────────────────────────────────────────────────

    AbstractMinecart::AbstractMinecart(EntityTypeId type, EntityLevel* level)
        : VehicleEntity(type, level) {
        m_displayOffset = 6;
    }

    AbstractMinecart::~AbstractMinecart() = default;

    glm::dvec3 AbstractMinecart::PassengerAttachmentAt(int slot, int total, bool animalPassenger,
                                                       float vehicleYRot) const {
        (void)slot; (void)total; (void)animalPassenger; (void)vehicleYRot;
        // EntityType .passengerAttachments(0.1875F) — the same point for
        // every passenger, (0, y, 0) turns to itself.
        return glm::dvec3(0.0, 0.1875, 0.0);
    }

    glm::dvec3 AbstractMinecart::GetPassengerAttachmentPoint(const Entity& passenger) const {
        // A villager or wandering trader sits lower (LOWERED_PASSENGER_ATTACHMENT).
        if (passenger.GetType() == EntityTypeId::Villager || passenger.GetType() == EntityTypeId::WanderingTrader) {
            return glm::dvec3(0.0);
        }
        return PassengerAttachmentAt(0, 1, false, yRot);
    }

    int AbstractMinecart::GetMotionDirectionIndex() const {
        // OldMinecartBehavior.getMotionDirection.
        const Direction dir = DirectionFromYRot(yRot);
        return static_cast<int>(m_flipped ? ClockWise(Opposite(dir)) : ClockWise(dir));
    }

    glm::dvec3 AbstractMinecart::GetDismountLocationForPassenger(const LivingEntity& passenger) const {
        // MC AbstractMinecart.getDismountLocationForPassenger.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return VehicleEntity::GetDismountLocationForPassenger(passenger);
        const Direction forward = static_cast<Direction>(GetMotionDirectionIndex());
        // DismountHelper.offsetsForDirection(forward).
        const Direction right = ClockWise(forward);
        const Direction left = Opposite(right);
        const Direction back = Opposite(forward);
        const auto sx = [](Direction d) { return StepX(d); };
        const auto sz = [](Direction d) { return StepZ(d); };
        const int offsets[8][2] = {
            {sx(right), sz(right)},
            {sx(left), sz(left)},
            {sx(back) + sx(right), sz(back) + sz(right)},
            {sx(back) + sx(left), sz(back) + sz(left)},
            {sx(forward) + sx(right), sz(forward) + sz(right)},
            {sx(forward) + sx(left), sz(forward) + sz(left)},
            {sx(back), sz(back)},
            {sx(forward), sz(forward)},
        };
        const glm::ivec3 vehicleBlockPos = BlockPosition();
        // getDismountPoses × POSE_DISMOUNT_HEIGHTS: STANDING/CROUCHING
        // (0, 1, -1), SWIMMING (0, 1).
        std::vector<double> heights;
        DismountPoseHeights(passenger, heights);
        const double width = PassengerWidth(passenger);
        const double reach = std::min(width, 1.0) / 2.0;
        for (size_t pose = 0; pose < heights.size(); ++pose) {
            const bool swimming = passenger.IsPlayer() && pose == 2;
            const int dys[3] = {0, 1, -1};
            const int nDy = swimming ? 2 : 3;
            for (int di = 0; di < nDy; ++di) {
                const int offsetY = dys[di];
                for (const auto& o : offsets) {
                    const glm::ivec3 target(vehicleBlockPos.x + o[0], vehicleBlockPos.y + offsetY,
                                            vehicleBlockPos.z + o[1]);
                    // DismountHelper.nonClimbableShape: a ladder or open
                    // trapdoor offers no floor. Climbables have no collision
                    // shape here except the scaffolding/trapdoor families.
                    const double floor = BlockFloorHeight(*blocks, target);
                    if (!IsBlockFloorValid(floor)) continue;
                    const glm::dvec3 location(target.x + 0.5, target.y + floor, target.z + 0.5);
                    const AABBd box = AABBd::FromMinMax(location - glm::dvec3(reach, 0.0, reach),
                                                        location + glm::dvec3(reach, heights[pose], reach));
                    if (CanDismountTo(*blocks, box)) return location;
                }
            }
        }
        return VehicleEntity::GetDismountLocationForPassenger(passenger);
    }

    float AbstractMinecart::GetBlockSpeedFactor() const {
        // MC AbstractMinecart.getBlockSpeedFactor: 1 on a rail.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (blocks) {
            const glm::ivec3 p = BlockPosition();
            if (IsRailAt(*blocks, p.x, p.y, p.z)) return 1.0f;
        }
        return VehicleEntity::GetBlockSpeedFactor();
    }

    glm::dvec3 AbstractMinecart::GetKnownMovement() const {
        // OldMinecartBehavior.getKnownMovement: capped to the track speed.
        const glm::dvec3 v = velocity;
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return glm::dvec3(0.0);
        return glm::dvec3(std::clamp(v.x, -0.4, 0.4), v.y, std::clamp(v.z, -0.4, 0.4));
    }

    glm::ivec3 AbstractMinecart::GetCurrentBlockPosOrRailBelow() const {
        int xt = FloorI(position.x), yt = FloorI(position.y), zt = FloorI(position.z);
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (blocks && IsRailAt(*blocks, xt, yt - 1, zt)) --yt;
        return glm::ivec3(xt, yt, zt);
    }

    bool AbstractMinecart::IsRedstoneConductor(const glm::ivec3& pos) const {
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        return blocks && Game::IsRedstoneConductor(*blocks, pos);
    }

    BlockState AbstractMinecart::GetDefaultDisplayBlockState() const {
        return BlockState{};   // Blocks.AIR
    }

    void AbstractMinecart::Tick() {
        // MC AbstractMinecart.tick.
        TickHurtAndDamage();
        // checkBelowWorld.
        if (m_level && position.y < static_cast<double>(m_level->GetMinY() - 64)) {
            Discard();
            return;
        }
        // (computeSpeed feeds the new movement's known speed; handlePortal is
        // the server's TickPortals.)
        if (IsPassenger() && GetVehicle()->IsRemoved()) StopRiding();
        if (m_boardingCooldown > 0) --m_boardingCooldown;
        BehaviorTick();
        if (IsRemoved()) return;
        // updateFluidInteraction — with the lava contact (ignite, hurt).
        UpdateInWaterStateAndDoFluidPushing();
        if (IsInLava()) fallDistance *= 0.5f;
        firstTick = false;
        AfterBehaviorTick();
    }

    void AbstractMinecart::BehaviorTick() {
        // OldMinecartBehavior.tick.
        if (!m_level) return;
        if (m_level->IsClientSide()) {
            // The client eases to the server's positions (the interpolation
            // is ClientMobManager's) and keeps the angles wrapped. Its motion
            // is what the rolling sounds read.
            velocity = position - oldPosition;
            xRot = std::fmod(xRot, 360.0f);
            yRot = std::fmod(yRot, 360.0f);
            return;
        }
        ApplyGravity();
        const glm::ivec3 pos = GetCurrentBlockPosOrRailBelow();
        const IBlockAccess* blocks = m_level->Blocks();
        const BlockState state = blocks ? blocks->GetBlockState(pos.x, pos.y, pos.z) : BlockState{};
        const bool onRails = IsRailBlock(state.Block());
        m_onRails = onRails;
        if (onRails) {
            MoveAlongTrack();
            if (IsRemoved()) return;
            if (state.Is(BlockID::ActivatorRail)) {
                ActivateMinecart(pos.x, pos.y, pos.z, PoweredOf(state));
                if (IsRemoved()) return;
            }
        } else {
            ComeOffTrack();
            if (IsRemoved()) return;
        }
        // applyEffectsFromBlocks: the server's per-tick inside-block pass
        // (detector rails, pressure plates, portals).
        xRot = 0.0f;
        const double xDiff = oldPosition.x - position.x;
        const double zDiff = oldPosition.z - position.z;
        if (xDiff * xDiff + zDiff * zDiff > 0.001) {
            yRot = static_cast<float>(std::atan2(zDiff, xDiff) * 180.0 / 3.141592653589793);
            if (m_flipped) yRot += 180.0f;
        }
        const double rotDiff = static_cast<double>(Mth::WrapDegrees(yRot - yRotO));
        if (rotDiff < -170.0 || rotDiff >= 170.0) {
            yRot += 180.0f;
            m_flipped = !m_flipped;
        }
        xRot = std::fmod(xRot, 360.0f);
        yRot = std::fmod(yRot, 360.0f);
        PushAndPickupEntities();
    }

    void AbstractMinecart::ComeOffTrack() {
        // MC AbstractMinecart.comeOffTrack.
        const double maxSpeed = GetMaxSpeed();
        velocity.x = std::clamp(velocity.x, -maxSpeed, maxSpeed);
        velocity.z = std::clamp(velocity.z, -maxSpeed, maxSpeed);
        if (onGround) velocity *= 0.5;
        MoveVehicle(velocity);
        if (!onGround) velocity *= static_cast<double>(GetAirDrag());
    }

    void AbstractMinecart::MoveAlongTrack() {
        // OldMinecartBehavior.moveAlongTrack, verbatim in order.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return;
        const glm::ivec3 pos = GetCurrentBlockPosOrRailBelow();
        const BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
        ResetFallDistance();
        double x = position.x;
        double y = position.y;
        double z = position.z;
        glm::dvec3 oldPos;
        const bool hasOldPos = GetRailPos(*blocks, x, y, z, oldPos);
        y = static_cast<double>(pos.y);
        bool powerTrack = false;
        bool haltTrack = false;
        if (state.Is(BlockID::PoweredRail)) {
            powerTrack = PoweredOf(state);
            haltTrack = !powerTrack;
        }
        double slideSpeed = 0.0078125;
        if (IsInWater()) slideSpeed *= 0.2;
        const RailShape shape = RailShapeOf(state);
        switch (shape) {
            case RailShape::AscendingEast:  velocity.x -= slideSpeed; ++y; break;
            case RailShape::AscendingWest:  velocity.x += slideSpeed; ++y; break;
            case RailShape::AscendingNorth: velocity.z += slideSpeed; ++y; break;
            case RailShape::AscendingSouth: velocity.z -= slideSpeed; ++y; break;
            default: break;
        }
        glm::dvec3 movement = velocity;
        const RailExits exits = ExitsOf(shape);
        const glm::ivec3 exit0 = exits.e0;
        const glm::ivec3 exit1 = exits.e1;
        double xD = static_cast<double>(exit1.x - exit0.x);
        double zD = static_cast<double>(exit1.z - exit0.z);
        const double length = std::sqrt(xD * xD + zD * zD);
        const double flip = movement.x * xD + movement.z * zD;
        if (flip < 0.0) {
            xD = -xD;
            zD = -zD;
        }
        const double pow = std::min(2.0, HorizontalLength(movement));
        movement = glm::dvec3(pow * xD / length, movement.y, pow * zD / length);
        velocity = movement;

        // The rider's keys: a player aboard a cart at rest nudges it along
        // the way they face (ServerPlayer.getLastClientMoveIntent).
        const Entity* controllingPassenger = GetPassengers().empty() ? nullptr : GetPassengers().front();
        if (controllingPassenger && controllingPassenger->IsPlayer()) {
            const glm::dvec3 moveIntent = GetLastClientMoveIntent(*controllingPassenger);
            if (glm::dot(moveIntent, moveIntent) > 0.0) {
                const double ownDist = HorizontalLengthSqr(velocity);
                if (ownDist < 0.01) {
                    velocity += glm::dvec3(moveIntent.x * 0.001, 0.0, moveIntent.z * 0.001);
                    haltTrack = false;
                }
            }
        }

        if (haltTrack) {
            const double speed = HorizontalLength(velocity);
            if (speed < 0.03) {
                velocity = glm::dvec3(0.0);
            } else {
                velocity.x *= 0.5;
                velocity.y = 0.0;
                velocity.z *= 0.5;
            }
        }

        const double x0 = static_cast<double>(pos.x) + 0.5 + static_cast<double>(exit0.x) * 0.5;
        const double z0 = static_cast<double>(pos.z) + 0.5 + static_cast<double>(exit0.z) * 0.5;
        const double x1 = static_cast<double>(pos.x) + 0.5 + static_cast<double>(exit1.x) * 0.5;
        const double z1 = static_cast<double>(pos.z) + 0.5 + static_cast<double>(exit1.z) * 0.5;
        xD = x1 - x0;
        zD = z1 - z0;
        double progress;
        if (xD == 0.0) {
            progress = z - static_cast<double>(pos.z);
        } else if (zD == 0.0) {
            progress = x - static_cast<double>(pos.x);
        } else {
            const double xx = x - x0;
            const double zz = z - z0;
            progress = (xx * xD + zz * zD) * 2.0;
        }
        x = x0 + xD * progress;
        z = z0 + zD * progress;
        position = glm::dvec3(x, y, z);

        const double riderFactor = (IsVehicle() || HasPlayerPassenger()) ? 0.75 : 1.0;
        const double maxSpeed = GetMaxSpeed();
        movement = velocity;
        MoveVehicle(glm::dvec3(std::clamp(riderFactor * movement.x, -maxSpeed, maxSpeed), 0.0,
                               std::clamp(riderFactor * movement.z, -maxSpeed, maxSpeed)));
        if (IsRemoved()) return;
        if (exit0.y != 0 && FloorI(position.x) - pos.x == exit0.x && FloorI(position.z) - pos.z == exit0.z) {
            position.y += static_cast<double>(exit0.y);
        } else if (exit1.y != 0 && FloorI(position.x) - pos.x == exit1.x &&
                   FloorI(position.z) - pos.z == exit1.z) {
            position.y += static_cast<double>(exit1.y);
        }

        velocity = ApplyNaturalSlowdown(velocity);
        glm::dvec3 newPos;
        if (GetRailPos(*blocks, position.x, position.y, position.z, newPos) && hasOldPos) {
            const double speed = (oldPos.y - newPos.y) * 0.05;
            const double speedLength = HorizontalLength(velocity);
            if (speedLength > 0.0) {
                const double k = (speedLength + speed) / speedLength;
                velocity.x *= k;
                velocity.z *= k;
            }
            position.y = newPos.y;
        }

        const int xn = FloorI(position.x);
        const int zn = FloorI(position.z);
        if (xn != pos.x || zn != pos.z) {
            const double speedLength = HorizontalLength(velocity);
            velocity = glm::dvec3(speedLength * static_cast<double>(xn - pos.x), velocity.y,
                                  speedLength * static_cast<double>(zn - pos.z));
        }

        if (powerTrack) {
            const double speedLength = HorizontalLength(velocity);
            if (speedLength > 0.01) {
                velocity += glm::dvec3(velocity.x / speedLength * 0.06, 0.0, velocity.z / speedLength * 0.06);
            } else {
                double dx = velocity.x;
                double dz = velocity.z;
                if (shape == RailShape::EastWest) {
                    if (IsRedstoneConductor(pos + glm::ivec3(-1, 0, 0)))      dx = 0.02;
                    else if (IsRedstoneConductor(pos + glm::ivec3(1, 0, 0))) dx = -0.02;
                } else {
                    if (shape != RailShape::NorthSouth) return;
                    if (IsRedstoneConductor(pos + glm::ivec3(0, 0, -1)))      dz = 0.02;
                    else if (IsRedstoneConductor(pos + glm::ivec3(0, 0, 1))) dz = -0.02;
                }
                velocity = glm::dvec3(dx, velocity.y, dz);
            }
        }
    }

    bool AbstractMinecart::GetRailPosOffs(const IBlockAccess& level, double x, double y, double z, double offs,
                                          glm::dvec3& out) {
        // OldMinecartBehavior.getPosOffs.
        const int xt = FloorI(x);
        int yt = FloorI(y);
        const int zt = FloorI(z);
        if (IsRailAt(level, xt, yt - 1, zt)) --yt;
        const BlockState state = level.GetBlockState(xt, yt, zt);
        if (!IsRailBlock(state.Block())) return false;
        const RailShape shape = RailShapeOf(state);
        y = static_cast<double>(yt);
        if (RailShapeIsSlope(shape)) y = static_cast<double>(yt + 1);
        const RailExits exits = ExitsOf(shape);
        double xD = static_cast<double>(exits.e1.x - exits.e0.x);
        double zD = static_cast<double>(exits.e1.z - exits.e0.z);
        const double dd = std::sqrt(xD * xD + zD * zD);
        xD /= dd;
        zD /= dd;
        x += xD * offs;
        z += zD * offs;
        if (exits.e0.y != 0 && FloorI(x) - xt == exits.e0.x && FloorI(z) - zt == exits.e0.z) {
            y += static_cast<double>(exits.e0.y);
        } else if (exits.e1.y != 0 && FloorI(x) - xt == exits.e1.x && FloorI(z) - zt == exits.e1.z) {
            y += static_cast<double>(exits.e1.y);
        }
        return GetRailPos(level, x, y, z, out);
    }

    bool AbstractMinecart::GetRailPos(const IBlockAccess& level, double x, double y, double z, glm::dvec3& out) {
        // OldMinecartBehavior.getPos.
        const int xt = FloorI(x);
        int yt = FloorI(y);
        const int zt = FloorI(z);
        if (IsRailAt(level, xt, yt - 1, zt)) --yt;
        const BlockState state = level.GetBlockState(xt, yt, zt);
        if (!IsRailBlock(state.Block())) return false;
        const RailShape shape = RailShapeOf(state);
        const RailExits exits = ExitsOf(shape);
        const double x0 = static_cast<double>(xt) + 0.5 + static_cast<double>(exits.e0.x) * 0.5;
        const double y0 = static_cast<double>(yt) + 0.0625 + static_cast<double>(exits.e0.y) * 0.5;
        const double z0 = static_cast<double>(zt) + 0.5 + static_cast<double>(exits.e0.z) * 0.5;
        const double x1 = static_cast<double>(xt) + 0.5 + static_cast<double>(exits.e1.x) * 0.5;
        const double y1 = static_cast<double>(yt) + 0.0625 + static_cast<double>(exits.e1.y) * 0.5;
        const double z1 = static_cast<double>(zt) + 0.5 + static_cast<double>(exits.e1.z) * 0.5;
        const double xD = x1 - x0;
        const double yD = (y1 - y0) * 2.0;
        const double zD = z1 - z0;
        double progress;
        if (xD == 0.0) {
            progress = z - static_cast<double>(zt);
        } else if (zD == 0.0) {
            progress = x - static_cast<double>(xt);
        } else {
            const double xx = x - x0;
            const double zz = z - z0;
            progress = (xx * xD + zz * zD) * 2.0;
        }
        x = x0 + xD * progress;
        y = y0 + yD * progress;
        z = z0 + zD * progress;
        if (yD < 0.0) {
            y += 1.0;
        } else if (yD > 0.0) {
            y += 0.5;
        }
        out = glm::dvec3(x, y, z);
        return true;
    }

    glm::dvec3 AbstractMinecart::ApplyNaturalSlowdown(const glm::dvec3& movement) const {
        // MC AbstractMinecart.applyNaturalSlowdown.
        const double slowdownFactor = GetSlowdownFactor();
        glm::dvec3 newMovement(movement.x * slowdownFactor, 0.0, movement.z * slowdownFactor);
        if (IsInWater()) newMovement *= 0.949999988079071;
        return newMovement;
    }

    bool AbstractMinecart::PushAndPickupEntities() {
        // OldMinecartBehavior.pushAndPickupEntities.
        if (!m_level) return false;
        const AABBd bb = GetAABBd();
        const AABB hitbox = AABB::FromMinMax(glm::vec3(bb.min - glm::dvec3(0.20000000298023224, 0.0, 0.20000000298023224)),
                                             glm::vec3(bb.max + glm::dvec3(0.20000000298023224, 0.0, 0.20000000298023224)));
        std::vector<Entity*> entities;
        m_level->GetEntitiesInBox(hitbox, this, entities);
        if (IsRideable() && HorizontalLengthSqr(velocity) >= 0.01) {
            // EntitySelector.pushableBy(this).
            std::erase_if(entities, [](Entity* e) { return !e || !e->IsPushable() || e->IsSpectator(); });
            for (Entity* entity : entities) {
                const bool isCart = IsMinecartEntityType(entity->GetType());
                if (!entity->IsPlayer() && entity->GetType() != EntityTypeId::IronGolem && !isCart &&
                    !IsVehicle() && !HasPlayerPassenger() && !entity->IsPassenger() &&
                    !IsVehicleEntityType(entity->GetType())) {
                    if (entity->StartRiding(*this)) {
                        entity->GameEvent(GameEventId::EntityMount, entity);
                    }
                } else if (isCart) {
                    static_cast<AbstractMinecart*>(entity)->PushEntity(*this);
                } else {
                    // entity.push(minecart): Entity.push(Entity) with the
                    // entity as `this`.
                    EntityPushPair(*entity, *this);
                }
            }
            return false;
        }
        for (Entity* entity : entities) {
            if (!entity || HasPassenger(*entity) || !entity->IsPushable()) continue;
            if (IsMinecartEntityType(entity->GetType())) {
                static_cast<AbstractMinecart*>(entity)->PushEntity(*this);
            }
        }
        return false;
    }

    void AbstractMinecart::PushEntity(Entity& entity) {
        // MC AbstractMinecart.push(Entity) — server only.
        if (!m_level || m_level->IsClientSide()) return;
        if (entity.IsSpectator()) return;   // noPhysics
        if (HasPassenger(entity)) return;
        double xa = entity.position.x - position.x;
        double za = entity.position.z - position.z;
        double dd = xa * xa + za * za;
        if (dd < 9.999999747378752E-5) return;
        dd = std::sqrt(dd);
        xa /= dd;
        za /= dd;
        double pow = 1.0 / dd;
        if (pow > 1.0) pow = 1.0;
        xa *= pow;
        za *= pow;
        xa *= 0.10000000149011612;
        za *= 0.10000000149011612;
        xa *= 0.5;
        za *= 0.5;
        if (IsMinecartEntityType(entity.GetType())) {
            PushOtherMinecart(static_cast<AbstractMinecart&>(entity), xa, za);
        } else {
            PushBy(-xa, 0.0, -za);
            // entity.push(xa / 4, 0, za / 4).
            if (!entity.IsPlayer()) {
                if (IsVehicleEntityType(entity.GetType())) {
                    static_cast<VehicleEntity&>(entity).PushBy(xa / 4.0, 0.0, za / 4.0);
                } else {
                    entity.velocity.x += xa / 4.0;
                    entity.velocity.z += za / 4.0;
                    entity.needsSync = true;
                    entity.physicsParked = false;
                }
            }
        }
    }

    void AbstractMinecart::PushOtherMinecart(AbstractMinecart& other, double xa, double za) {
        // MC AbstractMinecart.pushOtherMinecart (old movement).
        const double xo = other.position.x - position.x;
        const double zo = other.position.z - position.z;
        const double dirLen = std::sqrt(xo * xo + zo * zo);
        glm::dvec3 dir = dirLen < 1.0e-4 ? glm::dvec3(0.0) : glm::dvec3(xo / dirLen, 0.0, zo / dirLen);
        glm::dvec3 facing(std::cos(yRot * Mth::kDegToRad), 0.0, std::sin(yRot * Mth::kDegToRad));
        const double facingLen = glm::length(facing);
        if (facingLen > 1.0e-4) facing /= facingLen;
        const double dot = std::abs(glm::dot(dir, facing));
        if (dot < 0.800000011920929) return;
        const glm::dvec3 movement = velocity;
        const glm::dvec3 entityMovement = other.velocity;
        if (other.IsFurnace() && !IsFurnace()) {
            velocity = glm::dvec3(movement.x * 0.2, movement.y, movement.z * 0.2);
            PushBy(entityMovement.x - xa, 0.0, entityMovement.z - za);
            other.velocity = glm::dvec3(entityMovement.x * 0.95, entityMovement.y, entityMovement.z * 0.95);
            other.needsSync = true;
        } else if (!other.IsFurnace() && IsFurnace()) {
            other.velocity = glm::dvec3(entityMovement.x * 0.2, entityMovement.y, entityMovement.z * 0.2);
            other.PushBy(movement.x + xa, 0.0, movement.z + za);
            velocity = glm::dvec3(movement.x * 0.95, movement.y, movement.z * 0.95);
            needsSync = true;
        } else {
            const double xdd = (entityMovement.x + movement.x) / 2.0;
            const double zdd = (entityMovement.z + movement.z) / 2.0;
            velocity = glm::dvec3(movement.x * 0.2, movement.y, movement.z * 0.2);
            PushBy(xdd - xa, 0.0, zdd - za);
            other.velocity = glm::dvec3(entityMovement.x * 0.2, entityMovement.y, entityMovement.z * 0.2);
            other.PushBy(xdd + xa, 0.0, zdd + za);
        }
    }

    void AbstractMinecart::FillSyncedData(VehicleSyncedData& out) const {
        VehicleEntity::FillSyncedData(out);
        out.hasCustomDisplay = m_hasCustomDisplay;
        out.customDisplay = m_hasCustomDisplay ? m_customDisplay.RawId() : 0u;
        out.displayOffset = m_displayOffset;
    }

    void AbstractMinecart::ApplySyncedData(const VehicleSyncedData& in) {
        VehicleEntity::ApplySyncedData(in);
        m_hasCustomDisplay = in.hasCustomDisplay;
        m_customDisplay = in.hasCustomDisplay ? BlockState::FromRawId(in.customDisplay) : BlockState{};
        m_displayOffset = in.displayOffset;
    }

    // ── Minecart ───────────────────────────────────────────────────────────

    Minecart::Minecart(EntityLevel* level) : AbstractMinecart(EntityTypeId::Minecart, level) {}

    ItemID Minecart::GetDropItem() const { return Items::Minecart; }

    void Minecart::ActivateMinecart(int x, int y, int z, bool powered) {
        (void)x; (void)y; (void)z;
        // MC Minecart.activateMinecart: a powered activator rail throws the
        // rider off and shakes the cart (without breaking it).
        if (!powered) return;
        if (IsVehicle()) EjectPassengers();
        if (GetHurtTime() == 0) {
            SetHurtDir(-GetHurtDir());
            SetHurtTime(10);
            SetDamage(50.0f);
            hurtMarked = true;
        }
    }

    UseResult Minecart::VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) {
        (void)held;
        // MC Minecart.interact.
        if (sneaking || IsVehicle()) return UseResult::Pass;
        if (!m_level || m_level->IsClientSide()) return UseResult::Consume;
        return m_level->StartPlayerRiding(player, *this) ? UseResult::SuccessServer : UseResult::Consume;
    }

    // ── AbstractMinecartContainer ──────────────────────────────────────────

    MinecartContainerBase::MinecartContainerBase(EntityTypeId type, EntityLevel* level, int size)
        : AbstractMinecart(type, level), m_container(*this, size) {}

    void MinecartContainerBase::Destroy(MobDamageSource source, Entity* attacker) {
        // AbstractMinecartContainer.destroy: the cart (VehicleEntity), then
        // chestVehicleDestroyed.
        AbstractMinecart::Destroy(source, attacker);
        m_container.ChestVehicleDestroyed();
    }

    UseResult MinecartContainerBase::VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) {
        (void)held; (void)sneaking;
        // MC interactWithContainerVehicle: player.openMenu(this).
        if (!m_level || m_level->IsClientSide()) return UseResult::Success;
        if (m_level->OpenContainerEntityMenu(player, *this) && GetType() == EntityTypeId::ChestMinecart) {
            // MinecartChest.interact: CONTAINER_OPEN (the hopper cart's
            // interact is AbstractMinecartContainer's, with no event).
            GameEvent(GameEventId::ContainerOpen, &player);
        }
        return UseResult::Success;
    }

    void MinecartContainerBase::OnRemoving(RemovalReason reason) {
        if (!m_level || m_level->IsClientSide()) return;
        // RemovalReason.shouldDestroy: KILLED and DISCARDED.
        if (reason != RemovalReason::Killed && reason != RemovalReason::Discarded) return;
        m_container.DropContents();
    }

    void MinecartContainerBase::StopOpenContainer(LivingEntity& player) {
        if (GetType() != EntityTypeId::ChestMinecart || !m_level) return;
        if (ILevelWrite* world = m_level->MutableBlocks()) {
            world->GameEvent(&player, GameEventId::ContainerClose, position);
        }
    }

    glm::dvec3 MinecartContainerBase::ApplyNaturalSlowdown(const glm::dvec3& deltaMovement) const {
        // MC AbstractMinecartContainer.applyNaturalSlowdown: an emptier cart
        // rolls further (0.98 + emptiness / 1000), unless it still carries a
        // loot table.
        float keep = 0.98f;
        if (!m_container.HasContainerLootTable()) {
            const int emptiness = 15 - m_container.RedstoneSignal();
            keep += static_cast<float>(emptiness) * 0.001f;
        }
        if (IsInWater()) keep *= 0.95f;
        return glm::dvec3(deltaMovement.x * keep, 0.0, deltaMovement.z * keep);
    }

    // ── MinecartChest ──────────────────────────────────────────────────────

    MinecartChest::MinecartChest(EntityLevel* level)
        : MinecartContainerBase(EntityTypeId::ChestMinecart, level, 27) {
        SetDisplayOffset(GetDefaultDisplayOffset());
    }

    ItemID MinecartChest::GetDropItem() const { return Items::ChestMinecart; }

    BlockState MinecartChest::GetDefaultDisplayBlockState() const {
        // Blocks.CHEST, FACING north.
        return WithHorizontalFacing(BlockStates::Default(BlockID::Chest), Direction::North);
    }

    // ── MinecartHopper ─────────────────────────────────────────────────────

    MinecartHopper::MinecartHopper(EntityLevel* level)
        : MinecartContainerBase(EntityTypeId::HopperMinecart, level, 5) {
        SetDisplayOffset(GetDefaultDisplayOffset());
    }

    ItemID MinecartHopper::GetDropItem() const { return Items::HopperMinecart; }

    BlockState MinecartHopper::GetDefaultDisplayBlockState() const {
        return BlockStates::Default(BlockID::Hopper);
    }

    void MinecartHopper::ActivateMinecart(int x, int y, int z, bool powered) {
        (void)x; (void)y; (void)z;
        // MC MinecartHopper.activateMinecart: a powered activator rail locks it.
        const bool newEnabled = !powered;
        if (newEnabled != m_enabled) m_enabled = newEnabled;
    }

    void MinecartHopper::Tick() {
        // MC MinecartHopper.tick: consumedItemThisFrame = false; super.tick();
        // tryConsumeItems() (AfterBehaviorTick).
        m_consumedItemThisFrame = false;
        AbstractMinecart::Tick();
    }

    void MinecartHopper::TryConsumeItems() {
        if (!m_level || m_level->IsClientSide() || IsRemoved() || !m_enabled || m_consumedItemThisFrame) return;
        if (SuckInItems()) {
            m_consumedItemThisFrame = true;
            m_container.SetChanged();
        }
    }

    namespace {
        // HopperBlockEntity.tryTakeInItemFromSlot for a hopper that is a
        // minecart (the direction is DOWN).
        bool TryTakeInItemFromSlot(IContainer& hopper, IContainer& container, int slot) {
            const ItemStack stack = container.GetItem(slot);
            if (stack.IsEmpty()) return false;
            if (!container.CanTakeItem(hopper, slot, stack)) return false;
            if (const auto* worldly = dynamic_cast<const IWorldlyContainer*>(&container)) {
                if (!worldly->CanTakeItemThroughFace(slot, stack, Direction::Down)) return false;
            }
            const int originalCount = stack.count;
            const ItemStack result = HopperBlockEntity::AddItem(&container, hopper, container.RemoveItem(slot, 1),
                                                                Direction::Down, false);
            if (result.IsEmpty()) {
                container.SetChanged();
                return true;
            }
            ItemStack restored = stack;
            restored.count = originalCount;
            container.SetItem(slot, restored);
            return false;
        }

        // HopperBlockEntity.addItem(container, ItemEntity).
        bool AddItemEntityInto(EntityLevel& level, IContainer& hopper, int32_t itemEntityId) {
            const ItemStack* stack = level.GetItemEntityStack(itemEntityId);
            if (!stack || stack->IsEmpty()) return false;
            const ItemStack result = HopperBlockEntity::AddItem(nullptr, hopper, *stack, Direction::Down, false);
            level.SetItemEntityStack(itemEntityId, result);
            return result.IsEmpty();
        }
    } // namespace

    bool MinecartHopper::SuckInItems() {
        // MC MinecartHopper.suckInItems → HopperBlockEntity.suckInItems(level,
        // this), then the items within 0.25 of the cart.
        if (!m_level) return false;
        ILevelWrite* world = m_level->MutableBlocks();
        const glm::dvec3 levelPos = HopperLevelPos();
        const glm::ivec3 above(FloorI(levelPos.x), FloorI(levelPos.y + 1.0), FloorI(levelPos.z));
        if (world) {
            std::unique_ptr<IContainer> owned;
            IContainer* source = HopperBlockEntity::GetContainerAt(*world, above, owned);
            if (source && source != &m_container) {
                std::vector<int> slots;
                if (const auto* worldly = dynamic_cast<const IWorldlyContainer*>(source)) {
                    worldly->GetSlotsForFace(Direction::Down, slots);
                } else {
                    for (int i = 0; i < source->GetContainerSize(); ++i) slots.push_back(i);
                }
                for (int slot : slots) {
                    if (TryTakeInItemFromSlot(m_container, *source, slot)) return true;
                }
                return false;
            }
        }
        // Not grid aligned: never blocked. getItemsAtAndAbove: SUCK_AABB
        // (column 16 × 11..32) moved to the funnel's point.
        {
            const AABBd suck = AABBd::FromMinMax(
                glm::dvec3(levelPos.x - 0.5, levelPos.y - 0.5 + 11.0 / 16.0, levelPos.z - 0.5),
                glm::dvec3(levelPos.x + 0.5, levelPos.y - 0.5 + 2.0, levelPos.z + 0.5));
            std::vector<EntityLevel::NearbyItemEntity> items;
            m_level->GetItemEntitiesInBox(suck, items);
            for (const auto& item : items) {
                if (AddItemEntityInto(*m_level, m_container, item.id)) return true;
            }
        }
        // The cart's own box, inflated (0.25, 0, 0.25).
        {
            AABBd box = GetAABBd();
            box.min -= glm::dvec3(0.25, 0.0, 0.25);
            box.max += glm::dvec3(0.25, 0.0, 0.25);
            std::vector<EntityLevel::NearbyItemEntity> items;
            m_level->GetItemEntitiesInBox(box, items);
            for (const auto& item : items) {
                if (AddItemEntityInto(*m_level, m_container, item.id)) return true;
            }
        }
        return false;
    }

    // ── MinecartFurnace ────────────────────────────────────────────────────

    MinecartFurnace::MinecartFurnace(EntityLevel* level)
        : AbstractMinecart(EntityTypeId::FurnaceMinecart, level) {}

    ItemID MinecartFurnace::GetDropItem() const { return Items::FurnaceMinecart; }

    BlockState MinecartFurnace::GetDefaultDisplayBlockState() const {
        // Blocks.FURNACE, FACING north, LIT = hasFuel().
        return WithLit(WithHorizontalFacing(BlockStates::Default(BlockID::Furnace), Direction::North), m_hasFuel);
    }

    double MinecartFurnace::GetMaxSpeed() const {
        return IsInWater() ? AbstractMinecart::GetMaxSpeed() * 0.75 : AbstractMinecart::GetMaxSpeed() * 0.5;
    }

    void MinecartFurnace::Tick() {
        AbstractMinecart::Tick();
        if (!m_level || IsRemoved()) return;
        if (!m_level->IsClientSide()) {
            if (m_fuel > 0) --m_fuel;
            if (m_fuel <= 0) m_push = glm::dvec3(0.0);
            m_hasFuel = m_fuel > 0;
        }
        if (m_hasFuel && m_level->Random().NextInt(4) == 0) {
            m_level->AddParticle(ParticleOptions(ParticleKind::LargeSmoke), position.x, position.y + 0.8,
                                 position.z, 0.0, 0.0, 0.0);
        }
    }

    glm::dvec3 MinecartFurnace::ApplyNaturalSlowdown(const glm::dvec3& deltaMovement) const {
        // MC MinecartFurnace.applyNaturalSlowdown.
        glm::dvec3 newDeltaMovement;
        if (glm::dot(m_push, m_push) > 1.0E-7) {
            // calculateNewPushAlong: the push projected onto the motion,
            // keeping its length (the cart follows the rails round corners).
            glm::dvec3 push = m_push;
            const double pushH = m_push.x * m_push.x + m_push.z * m_push.z;
            const double moveH = deltaMovement.x * deltaMovement.x + deltaMovement.z * deltaMovement.z;
            if (pushH > 1.0E-4 && moveH > 0.001) {
                const double len2 = glm::dot(deltaMovement, deltaMovement);
                glm::dvec3 projected = deltaMovement * (glm::dot(m_push, deltaMovement) / len2);
                const double pl = glm::length(projected);
                projected = pl < 1.0E-5 ? glm::dvec3(0.0) : projected / pl;
                push = projected * glm::length(m_push);
            }
            const_cast<MinecartFurnace*>(this)->m_push = push;
            newDeltaMovement = glm::dvec3(deltaMovement.x * 0.8, 0.0, deltaMovement.z * 0.8) + push;
            if (IsInWater()) newDeltaMovement *= 0.1;
        } else {
            newDeltaMovement = glm::dvec3(deltaMovement.x * 0.98, 0.0, deltaMovement.z * 0.98);
        }
        return AbstractMinecart::ApplyNaturalSlowdown(newDeltaMovement);
    }

    bool MinecartFurnace::AddFuel(const glm::dvec3& interactingPos, const ItemStack& itemStack) {
        // MC addFuel.
        if (itemStack.IsEmpty() || !IsFurnaceMinecartFuel(itemStack.itemId)) return false;
        if (m_fuel + kFuelTicksPerItem > kMaxFuelTicks) return false;
        m_fuel += kFuelTicksPerItem;
        if (m_fuel > 0) {
            const glm::dvec3 d = position - interactingPos;
            m_push = glm::dvec3(d.x, 0.0, d.z);   // .horizontal()
        }
        return true;
    }

    UseResult MinecartFurnace::VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) {
        (void)sneaking;
        // MC MinecartFurnace.interact: fuel pushes it away from the player;
        // the item is consumed (not in creative).
        if (m_level && !m_level->IsClientSide() && AddFuel(player.position, held)) {
            if (!player.IsCreative()) {
                held.count -= 1;
                if (held.count <= 0) held.Clear();
            }
        }
        return UseResult::Success;
    }

    void MinecartFurnace::FillSyncedData(VehicleSyncedData& out) const {
        AbstractMinecart::FillSyncedData(out);
        out.hasFuel = m_hasFuel;
    }

    void MinecartFurnace::ApplySyncedData(const VehicleSyncedData& in) {
        AbstractMinecart::ApplySyncedData(in);
        m_hasFuel = in.hasFuel;
    }

    // ── MinecartTNT ────────────────────────────────────────────────────────

    MinecartTNT::MinecartTNT(EntityLevel* level) : AbstractMinecart(EntityTypeId::TntMinecart, level) {}

    ItemID MinecartTNT::GetDropItem() const { return Items::TntMinecart; }

    BlockState MinecartTNT::GetDefaultDisplayBlockState() const { return BlockStates::Default(BlockID::Tnt); }

    void MinecartTNT::SetExplosionPowerBase(float v) { m_explosionPowerBase = std::clamp(v, 0.0f, 128.0f); }
    void MinecartTNT::SetExplosionSpeedFactor(float v) { m_explosionSpeedFactor = std::clamp(v, 0.0f, 128.0f); }

    uint8_t MinecartTNT::GetAnimStateByte() const {
        // 0 = not primed, fuse + 1 otherwise (the fuse runs 0..80).
        return static_cast<uint8_t>(m_fuse < 0 ? 0 : std::min(m_fuse + 1, 255));
    }

    void MinecartTNT::SetAnimStateByte(uint8_t v) {
        m_fuse = static_cast<int>(v) - 1;
    }

    void MinecartTNT::Tick() {
        AbstractMinecart::Tick();
        if (!m_level || IsRemoved()) return;
        // MC MinecartTNT.tick.
        if (m_fuse > 0) {
            --m_fuse;
            m_level->AddParticle(ParticleOptions(ParticleKind::Smoke), position.x, position.y + 0.5, position.z,
                                 0.0, 0.0, 0.0);
        } else if (m_fuse == 0) {
            Explode(HorizontalLengthSqr(velocity));
            if (IsRemoved()) return;
        }
        if (horizontalCollision) {
            const double speedSqr = HorizontalLengthSqr(velocity);
            if (speedSqr >= 0.009999999776482582) Explode(speedSqr);
        }
    }

    bool MinecartTNT::DamageSourceIgnitesTnt(MobDamageSource source, const Entity* direct) {
        // MC damageSourceIgnitesTnt: a burning projectile, fire or a blast.
        if (const auto* projectile = dynamic_cast<const Projectile*>(direct)) return projectile->IsOnFire();
        return source == MobDamageSource::Fire || source == MobDamageSource::Lava ||
               source == MobDamageSource::Explosion;
    }

    bool MinecartTNT::ShouldSourceDestroy(MobDamageSource source, const Entity* direct) const {
        return DamageSourceIgnitesTnt(source, direct);
    }

    bool MinecartTNT::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC MinecartTNT.hurtServer: a flaming arrow sets it off at its own
        // speed first.
        const Entity* direct = HurtDirectEntity();
        if (const auto* arrow = dynamic_cast<const Projectile*>(direct);
            arrow && source == MobDamageSource::Projectile && arrow->IsOnFire()) {
            if (!m_igniter) m_igniter = attacker;
            Explode(glm::dot(arrow->velocity, arrow->velocity));
            if (IsRemoved()) return true;
        }
        return AbstractMinecart::Hurt(source, amount, attacker);
    }

    void MinecartTNT::Destroy(MobDamageSource source, Entity* attacker) {
        // MC MinecartTNT.destroy.
        const double speedSqr = HorizontalLengthSqr(velocity);
        const Entity* direct = HurtDirectEntity() ? HurtDirectEntity() : attacker;
        if (!DamageSourceIgnitesTnt(source, direct) && !(speedSqr >= 0.009999999776482582)) {
            DestroyWithItem(GetDropItem());
            return;
        }
        if (m_fuse < 0) {
            PrimeFuse(attacker);
            JavaRandom& rng = m_level->Random();
            m_fuse = rng.NextInt(20) + rng.NextInt(20);
        }
    }

    namespace {
        // MC MinecartTNT.getBlockExplosionResistance / shouldBlockExplode: a
        // PRIMED cart's blast spares the rails and whatever they lie on.
        bool CartSparesBlock(const ExplosionParams& p, const glm::ivec3& pos, BlockState state) {
            const auto* cart = dynamic_cast<const MinecartTNT*>(p.source);
            if (!cart || !cart->IsPrimed()) return false;
            if (IsRailBlock(state.Block())) return true;
            const EntityLevel* level = cart->Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            return blocks && IsRailBlock(blocks->GetBlock(pos.x, pos.y + 1, pos.z));
        }
        bool CartBlockResistance(const ExplosionParams& p, const glm::ivec3& pos, BlockState state, float& out) {
            if (!CartSparesBlock(p, pos, state)) return false;
            out = 0.0f;
            return true;
        }
        bool CartShouldBlockExplode(const ExplosionParams& p, const glm::ivec3& pos, BlockState state, float power) {
            (void)power;
            return !CartSparesBlock(p, pos, state);
        }
    } // namespace

    void MinecartTNT::Explode(double speedSqr) {
        // MC MinecartTNT.explode.
        if (!m_level || m_level->IsClientSide() || IsRemoved()) return;
        if (m_level->TntExplodes()) {
            const double speed = std::min(std::sqrt(speedSqr), 5.0);
            ExplosionParams params;
            params.center = position;
            params.radius = static_cast<float>(static_cast<double>(m_explosionPowerBase) +
                                               static_cast<double>(m_explosionSpeedFactor) *
                                                   m_level->Random().NextDouble() * 1.5 * speed);
            params.source = this;
            params.attributedTo = m_igniter;
            params.interaction = ExplosionInteraction::Tnt;
            params.fire = false;
            params.calculator.blockResistance = &CartBlockResistance;
            params.calculator.shouldBlockExplode = &CartShouldBlockExplode;
            m_level->QueueExplosion(params);
            Discard();
        } else if (IsPrimed()) {
            Discard();
        }
    }

    bool MinecartTNT::CauseFallDamage(double fallDist, float damageMultiplier) {
        (void)damageMultiplier;
        // MC MinecartTNT.causeFallDamage: a fall of 3+ blows it up.
        if (fallDist >= 3.0) {
            const double power = fallDist / 10.0;
            Explode(power * power);
        }
        return false;
    }

    void MinecartTNT::ActivateMinecart(int x, int y, int z, bool powered) {
        (void)x; (void)y; (void)z;
        if (powered && m_fuse < 0) PrimeFuse(nullptr);
    }

    void MinecartTNT::HandleEntityEvent(uint8_t id) {
        if (id == 70) {
            // The client's copy starts its own fuse (the swell and flash).
            m_fuse = 80;
            return;
        }
        AbstractMinecart::HandleEntityEvent(id);
    }

    void MinecartTNT::ClearReferenceTo(const Entity* entity) {
        AbstractMinecart::ClearReferenceTo(entity);
        if (m_igniter == entity) m_igniter = nullptr;
    }

    void MinecartTNT::PrimeFuse(Entity* igniter) {
        // MC primeFuse(source).
        if (!m_level) return;
        if (!m_level->IsClientSide() && !m_level->TntExplodes()) return;
        m_fuse = 80;
        if (!m_level->IsClientSide()) {
            if (igniter && !m_igniter) {
                m_igniter = igniter;
                MarkHoldsEntityRefs();
            }
            m_level->BroadcastEntityEvent(*this, 70);
            if (!IsSilent()) {
                m_level->PlaySound(nullptr, position, SoundEvents::TNT_PRIMED, SoundSource::Blocks, 1.0f, 1.0f);
            }
            GameEvent(GameEventId::PrimeFuse, igniter);
        }
    }

    // ── MinecartSpawner ────────────────────────────────────────────────────

    namespace {
        // The spawner logic at the cart's cell: SpawnerBlockEntity's
        // BaseSpawner, re-pointed each tick (MC's anonymous BaseSpawner with
        // blockPosition()).
        class CartSpawnerLogic final : public SpawnerBlockEntity {
        public:
            CartSpawnerLogic()
                : SpawnerBlockEntity(BlockEntityTypes::ForBlock(BlockID::Spawner), glm::ivec3(0), BlockID::Spawner) {}
            void MoveTo(const glm::ivec3& pos) { MoveCarriedTo(pos); }
        };
    } // namespace

    MinecartSpawner::MinecartSpawner(EntityLevel* level)
        : AbstractMinecart(EntityTypeId::SpawnerMinecart, level),
          m_spawner(std::make_unique<CartSpawnerLogic>()) {}

    MinecartSpawner::~MinecartSpawner() = default;

    ItemID MinecartSpawner::GetDropItem() const { return Items::Minecart; }
    ItemStack MinecartSpawner::GetPickResult() const { return ItemStack(Items::Minecart, 1); }

    BlockState MinecartSpawner::GetDefaultDisplayBlockState() const { return BlockStates::Default(BlockID::Spawner); }

    void MinecartSpawner::Tick() {
        AbstractMinecart::Tick();
        if (!m_level || IsRemoved() || m_level->IsClientSide()) return;
        // spawner.serverTick(level, blockPosition()).
        auto* logic = static_cast<CartSpawnerLogic*>(m_spawner.get());
        logic->MoveTo(BlockPosition());
        if (auto* world = dynamic_cast<World*>(m_level->MutableBlocks())) logic->Tick(world, 0.05f);
    }

    // ── MinecartCommandBlock ───────────────────────────────────────────────

    namespace {
        std::atomic<MinecartCommandBlock::CommandRunner> s_commandRunner{nullptr};
    }

    void MinecartCommandBlock::SetCommandRunner(CommandRunner runner) {
        s_commandRunner.store(runner, std::memory_order_release);
    }

    MinecartCommandBlock::MinecartCommandBlock(EntityLevel* level)
        : AbstractMinecart(EntityTypeId::CommandBlockMinecart, level) {}

    ItemID MinecartCommandBlock::GetDropItem() const { return Items::Minecart; }
    ItemStack MinecartCommandBlock::GetPickResult() const { return ItemStack(Items::CommandBlockMinecart, 1); }

    BlockState MinecartCommandBlock::GetDefaultDisplayBlockState() const {
        return BlockStates::Default(BlockID::CommandBlock);
    }

    void MinecartCommandBlock::ActivateMinecart(int x, int y, int z, bool powered) {
        (void)x; (void)y; (void)z;
        // MC activateMinecart: at most once every ACTIVATION_DELAY ticks.
        if (powered && tickCount - m_lastActivated >= kActivationDelay) {
            PerformCommand();
            m_lastActivated = tickCount;
        }
    }

    bool MinecartCommandBlock::PerformCommand() {
        // MC BaseCommandBlock.performCommand.
        if (!m_level || m_level->IsClientSide()) return false;
        const int64_t now = m_level->GetGameTime();
        if (now == m_lastExecution) return false;
        if (m_command.size() == 6) {
            std::string lower = m_command;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "searge") {
                m_lastOutput = "#itzlipofutzli";
                m_successCount = 1;
                return true;
            }
        }
        m_successCount = 0;
        // level.isCommandBlockEnabled — the command_blocks_work rule.
        if (Rules::GetBool(Rules::Id::CommandBlocksWork) && !m_command.empty()) {
            m_lastOutput.clear();
            if (const CommandRunner run = s_commandRunner.load(std::memory_order_acquire)) {
                std::string output;
                m_successCount = run(*this, m_command, output);
                if (m_trackOutput) m_lastOutput = output;
            }
        }
        m_lastExecution = m_updateLastExecution ? now : -1;
        return true;
    }

    // ── Factory ────────────────────────────────────────────────────────────

    std::unique_ptr<AbstractMinecart> CreateMinecart(EntityTypeId type, EntityLevel* level) {
        switch (type) {
            case EntityTypeId::Minecart:             return std::make_unique<Minecart>(level);
            case EntityTypeId::ChestMinecart:        return std::make_unique<MinecartChest>(level);
            case EntityTypeId::FurnaceMinecart:      return std::make_unique<MinecartFurnace>(level);
            case EntityTypeId::HopperMinecart:       return std::make_unique<MinecartHopper>(level);
            case EntityTypeId::TntMinecart:          return std::make_unique<MinecartTNT>(level);
            case EntityTypeId::SpawnerMinecart:      return std::make_unique<MinecartSpawner>(level);
            case EntityTypeId::CommandBlockMinecart: return std::make_unique<MinecartCommandBlock>(level);
            default:                                 return nullptr;
        }
    }

} // namespace Game
