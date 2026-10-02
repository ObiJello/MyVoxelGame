// File: src/common/entity/HorseTaming.cpp
#include "common/entity/HorseTaming.hpp"
#include "server/advancements/CriteriaTriggers.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Game {

    int HorseTaming::ModifyTemper(int amount) {
        const int temper = std::clamp(GetTemper() + amount, 0, GetMaxTemper());
        SetTemper(temper);
        return temper;
    }

    void HorseTaming::SetOwner(const LivingEntity* owner) {
        m_ownerRef.Set(owner);
    }

    LivingEntity* HorseTaming::GetOwner() const {
        EntityLevel* level = m_horseSelf ? m_horseSelf->Level() : nullptr;
        if (!level) return nullptr;
        return const_cast<EntityRef&>(m_ownerRef).GetLiving(*level);
    }

    bool HorseTaming::TameWithName(const LivingEntity& player) {
        SetOwner(&player);
        SetTamed(true);
        // AbstractHorse.tameWithName: CriteriaTriggers.TAME_ANIMAL.
        if (m_horseSelf && m_horseSelf->Level() && !m_horseSelf->Level()->IsClientSide()) {
            if (Server::ServerPlayer* sp = Server::CriteriaTriggers::PlayerOf(&player)) {
                Server::CriteriaTriggers::TameAnimal(*sp, *m_horseSelf);
            }
        }
        if (EntityLevel* level = m_horseSelf ? m_horseSelf->Level() : nullptr) {
            level->BroadcastEntityEvent(*m_horseSelf, 7);
        }
        return true;
    }

    bool HorseTaming::IsEquineVehicle() const {
        return m_horseSelf && m_horseSelf->IsVehicle();
    }

    LivingEntity* HorseTaming::GetPlayerRider() const {
        return m_horseSelf ? PlayerRideable::FirstPlayerPassenger(*m_horseSelf) : nullptr;
    }

    bool HorseTaming::HasExactlyOnePlayerPassenger() const {
        if (!m_horseSelf) return false;
        const auto& riders = m_horseSelf->GetPassengers();
        return riders.size() == 1 && riders.front() && riders.front()->IsPlayer();
    }

    void HorseTaming::EjectPlayerRider() {
        if (m_horseSelf) PlayerRideable::EjectPlayerPassengers(*m_horseSelf);
    }

    bool HorseTaming::StartPlayerRide(LivingEntity& player) {
        if (!m_horseSelf) return false;
        EntityLevel* level = m_horseSelf->Level();
        if (!level || level->IsClientSide()) return false;
        return level->StartPlayerRiding(player, *m_horseSelf);
    }

    void HorseTaming::SpawnTamingParticles(bool success) const {
        // MC AbstractHorse.spawnTamingParticles: 7 particles, HEART or SMOKE,
        // gaussian * 0.02 velocities at getRandomX(1.0) / getRandomY() + 0.5
        // / getRandomZ(1.0), Java argument order (xa, ya, za, then position).
        if (!m_horseSelf) return;
        EntityLevel* level = m_horseSelf->Level();
        if (!level) return;
        const ParticleKind kind = success ? ParticleKind::Heart : ParticleKind::Smoke;
        JavaRandom& rng = level->Random();
        const double w = static_cast<double>(m_horseSelf->GetBbWidth());
        const double h = static_cast<double>(m_horseSelf->GetBbHeight());
        for (int i = 0; i < 7; ++i) {
            const double xa = rng.NextGaussian() * 0.02;
            const double ya = rng.NextGaussian() * 0.02;
            const double za = rng.NextGaussian() * 0.02;
            const double px = m_horseSelf->position.x + w * (2.0 * rng.NextDouble() - 1.0);
            const double py = m_horseSelf->position.y + h * rng.NextDouble() + 0.5;
            const double pz = m_horseSelf->position.z + w * (2.0 * rng.NextDouble() - 1.0);
            level->AddParticle(kind, px, py, pz, xa, ya, za);
        }
    }

    bool HorseTaming::IsSecondaryUseActive(const LivingEntity& player) {
        // MC isSecondaryUseActive = isShiftKeyDown: the server's sneak state
        // for the player behind the view.
        const EntityLevel* level = player.Level();
        return level && level->IsPlayerSneaking(player);
    }

} // namespace Game

namespace Game {

    LivingEntity* PlayerRideable::FirstPlayerPassenger(const Entity& self) {
        const auto& riders = self.GetPassengers();
        if (riders.empty() || !riders.front() || !riders.front()->IsPlayer()) return nullptr;
        return riders.front()->AsLiving();
    }

    bool PlayerRideable::HasPlayerPassenger(const Entity& self) {
        for (const Entity* p : self.GetPassengers()) {
            if (p && p->IsPlayer()) return true;
        }
        return false;
    }

    void PlayerRideable::EjectPlayerPassengers(Entity& self) {
        // Back to front like Entity::EjectPassengers; a copy, since each
        // StopRiding edits the list.
        const std::vector<Entity*> riders = self.GetPassengers();
        for (size_t i = riders.size(); i-- > 0;) {
            if (riders[i] && riders[i]->IsPlayer()) riders[i]->StopRiding();
        }
    }

} // namespace Game

namespace Game {

    namespace {
        // MC AbstractHorse.getDismountLocationInDirection.
        std::optional<glm::dvec3> EquineDismountInDirection(const Mob& mount, const glm::dvec3& direction,
                                                            const LivingEntity& passenger) {
            const EntityLevel* level = mount.Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            if (!blocks) return std::nullopt;
            const double targetX = mount.position.x + direction.x;
            const double targetY = mount.GetAABBd().min.y;
            const double targetZ = mount.position.z + direction.z;
            const double dismountJumpLimit = mount.GetAABBd().max.y + 0.75;
            const double halfWidth = VehicleEntity::PassengerWidth(passenger) * 0.5;
            std::vector<double> heights;
            VehicleEntity::DismountPoseHeights(passenger, heights);
            for (double height : heights) {
                glm::ivec3 target(static_cast<int>(std::floor(targetX)), static_cast<int>(std::floor(targetY)),
                                  static_cast<int>(std::floor(targetZ)));
                while (true) {
                    const double floor = VehicleEntity::BlockFloorHeight(*blocks, target);
                    if (static_cast<double>(target.y) + floor > dismountJumpLimit) break;
                    if (VehicleEntity::IsBlockFloorValid(floor)) {
                        const glm::dvec3 location(targetX, static_cast<double>(target.y) + floor, targetZ);
                        const AABBd box = AABBd::FromMinMax(location - glm::dvec3(halfWidth, 0.0, halfWidth),
                                                            location + glm::dvec3(halfWidth, height, halfWidth));
                        if (VehicleEntity::CanDismountTo(*blocks, box)) return location;
                    }
                    ++target.y;
                    if (!(static_cast<double>(target.y) < dismountJumpLimit)) break;
                }
            }
            return std::nullopt;
        }
    } // namespace

    float HorseTaming::GenerateMaxHealth(JavaRandom& rng) {
        // MC: 15.0F + (float)nextInt(8) + (float)nextInt(9).
        const float a = static_cast<float>(rng.NextInt(8));
        const float b = static_cast<float>(rng.NextInt(9));
        return 15.0f + a + b;
    }

    double HorseTaming::GenerateJumpStrength(JavaRandom& rng) {
        const double a = rng.NextDouble();
        const double b = rng.NextDouble();
        const double c = rng.NextDouble();
        return 0.4000000059604645 + a * 0.2 + b * 0.2 + c * 0.2;
    }

    double HorseTaming::GenerateSpeed(JavaRandom& rng) {
        const double a = rng.NextDouble();
        const double b = rng.NextDouble();
        const double c = rng.NextDouble();
        return (0.44999998807907104 + a * 0.3 + b * 0.3 + c * 0.3) * 0.25;
    }

    double HorseTaming::CreateOffspringAttribute(double parentA, double parentB, double min, double max,
                                                 JavaRandom& rng) {
        // MC throws on an empty range; every caller passes a real one.
        if (max <= min) return min;
        parentA = std::clamp(parentA, min, max);
        parentB = std::clamp(parentB, min, max);
        const double margin = 0.15 * (max - min);
        const double range = std::abs(parentA - parentB) + margin * 2.0;
        const double average = (parentA + parentB) / 2.0;
        const double r1 = rng.NextDouble();
        const double r2 = rng.NextDouble();
        const double r3 = rng.NextDouble();
        const double babyQuality = (r1 + r2 + r3) / 3.0 - 0.5;
        const double value = average + range * babyQuality;
        if (value > max) return max - (value - max);
        if (value < min) return min + (min - value);
        return value;
    }

    void HorseTaming::SetOffspringAttributes(const Mob& self, const Mob& partner, Mob& baby) {
        EntityLevel* level = self.Level();
        if (!level) return;
        JavaRandom& rng = level->Random();
        // MC's static MIN_/MAX_ fields: the generators at 0 and at 1, each
        // narrowed to float as MC stores them.
        const double minHealth = static_cast<double>(15.0f);
        const double maxHealth = static_cast<double>(15.0f + 7.0f + 8.0f);
        const double minJump = static_cast<double>(static_cast<float>(0.4000000059604645));
        const double maxJump = static_cast<double>(static_cast<float>(0.4000000059604645 + 0.2 + 0.2 + 0.2));
        const double minSpeed = static_cast<double>(static_cast<float>(0.44999998807907104 * 0.25));
        const double maxSpeed = static_cast<double>(static_cast<float>((0.44999998807907104 + 0.3 + 0.3 + 0.3) * 0.25));
        const auto inherit = [&](Attribute attr, double min, double max) {
            const double v = CreateOffspringAttribute(self.Attributes().GetBaseValue(attr),
                                                      partner.Attributes().GetBaseValue(attr), min, max, rng);
            baby.Attributes().SetBaseValue(attr, v);
        };
        inherit(Attribute::MaxHealth, minHealth, maxHealth);
        inherit(Attribute::JumpStrength, minJump, maxJump);
        inherit(Attribute::MovementSpeed, minSpeed, maxSpeed);
        // LivingEntity.onAttributeUpdated: health never exceeds the new max.
        if (baby.GetHealth() > baby.GetMaxHealth()) baby.SetHealth(baby.GetMaxHealth());
    }

    glm::dvec3 HorseTaming::EquineDismountLocation(const Mob& mount, const LivingEntity& passenger) {
        // HumanoidArm: a player's main arm is the right one; a mob's follows
        // its left-handed flag.
        const auto* mobPassenger = dynamic_cast<const Mob*>(&passenger);
        const bool rightHanded = passenger.IsPlayer() || !mobPassenger || !mobPassenger->IsLeftHanded();
        const double mountWidth = mount.GetBbWidth();
        const double riderWidth = VehicleEntity::PassengerWidth(passenger);
        const glm::dvec3 mainHand = VehicleEntity::CollisionHorizontalEscapeVector(
            mountWidth, riderWidth, mount.yRot + (rightHanded ? 90.0f : -90.0f));
        if (auto at = EquineDismountInDirection(mount, mainHand, passenger)) return *at;
        const glm::dvec3 offHand = VehicleEntity::CollisionHorizontalEscapeVector(
            mountWidth, riderWidth, mount.yRot + (rightHanded ? -90.0f : 90.0f));
        if (auto at = EquineDismountInDirection(mount, offHand, passenger)) return *at;
        return mount.position;
    }

} // namespace Game
