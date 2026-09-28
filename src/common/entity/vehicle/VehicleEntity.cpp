// File: src/common/entity/vehicle/VehicleEntity.cpp
#include "common/entity/vehicle/VehicleEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/ArmorStand.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/PlayerRideable.hpp"
#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/block/BlockBounce.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace Game {

    // ── Types ──────────────────────────────────────────────────────────────

    bool IsBoatEntityType(EntityTypeId type) {
        const auto t = static_cast<uint16_t>(type);
        return t >= static_cast<uint16_t>(EntityTypeId::OakBoat) &&
               t <= static_cast<uint16_t>(EntityTypeId::PoplarChestBoat);
    }

    bool IsChestBoatEntityType(EntityTypeId type) {
        switch (type) {
            case EntityTypeId::OakChestBoat:     case EntityTypeId::SpruceChestBoat:
            case EntityTypeId::BirchChestBoat:   case EntityTypeId::JungleChestBoat:
            case EntityTypeId::AcaciaChestBoat:  case EntityTypeId::CherryChestBoat:
            case EntityTypeId::DarkOakChestBoat: case EntityTypeId::PaleOakChestBoat:
            case EntityTypeId::MangroveChestBoat:case EntityTypeId::BambooChestRaft:
            case EntityTypeId::PoplarChestBoat:
                return true;
            default:
                return false;
        }
    }

    bool IsRaftEntityType(EntityTypeId type) {
        return type == EntityTypeId::BambooRaft || type == EntityTypeId::BambooChestRaft;
    }

    bool IsMinecartEntityType(EntityTypeId type) {
        const auto t = static_cast<uint16_t>(type);
        return t >= static_cast<uint16_t>(EntityTypeId::Minecart) &&
               t <= static_cast<uint16_t>(EntityTypeId::CommandBlockMinecart);
    }

    namespace {
        struct BoatItemPair { ItemID item; EntityTypeId type; };
        // EntityTypes' boatFactory / chestBoatFactory / raftFactory item
        // suppliers — the boat an item places is the boat that drops it.
        const BoatItemPair kBoatItems[] = {
            {Items::OakBoat,           EntityTypeId::OakBoat},
            {Items::OakChestBoat,      EntityTypeId::OakChestBoat},
            {Items::SpruceBoat,        EntityTypeId::SpruceBoat},
            {Items::SpruceChestBoat,   EntityTypeId::SpruceChestBoat},
            {Items::BirchBoat,         EntityTypeId::BirchBoat},
            {Items::BirchChestBoat,    EntityTypeId::BirchChestBoat},
            {Items::JungleBoat,        EntityTypeId::JungleBoat},
            {Items::JungleChestBoat,   EntityTypeId::JungleChestBoat},
            {Items::AcaciaBoat,        EntityTypeId::AcaciaBoat},
            {Items::AcaciaChestBoat,   EntityTypeId::AcaciaChestBoat},
            {Items::CherryBoat,        EntityTypeId::CherryBoat},
            {Items::CherryChestBoat,   EntityTypeId::CherryChestBoat},
            {Items::DarkOakBoat,       EntityTypeId::DarkOakBoat},
            {Items::DarkOakChestBoat,  EntityTypeId::DarkOakChestBoat},
            {Items::PaleOakBoat,       EntityTypeId::PaleOakBoat},
            {Items::PaleOakChestBoat,  EntityTypeId::PaleOakChestBoat},
            {Items::MangroveBoat,      EntityTypeId::MangroveBoat},
            {Items::MangroveChestBoat, EntityTypeId::MangroveChestBoat},
            {Items::BambooRaft,        EntityTypeId::BambooRaft},
            {Items::BambooChestRaft,   EntityTypeId::BambooChestRaft},
            {Items::PoplarBoat,        EntityTypeId::PoplarBoat},
            {Items::PoplarChestBoat,   EntityTypeId::PoplarChestBoat},
        };
    } // namespace

    EntityTypeId BoatTypeForItem(ItemID item) {
        for (const BoatItemPair& p : kBoatItems) if (p.item == item) return p.type;
        return EntityTypeId::Count;
    }

    ItemID BoatItemForType(EntityTypeId type) {
        for (const BoatItemPair& p : kBoatItems) if (p.type == type) return p.item;
        return Items::Air;
    }

    EntityTypeId MinecartTypeForItem(ItemID item) {
        // MinecartItem(type) per item (Items.java).
        if (item == Items::Minecart)             return EntityTypeId::Minecart;
        if (item == Items::ChestMinecart)        return EntityTypeId::ChestMinecart;
        if (item == Items::FurnaceMinecart)      return EntityTypeId::FurnaceMinecart;
        if (item == Items::TntMinecart)          return EntityTypeId::TntMinecart;
        if (item == Items::HopperMinecart)       return EntityTypeId::HopperMinecart;
        if (item == Items::CommandBlockMinecart) return EntityTypeId::CommandBlockMinecart;
        return EntityTypeId::Count;
    }

    // ── The riding player's keys ───────────────────────────────────────────

    namespace {
        std::atomic<VehicleEntity::PlayerInputFn> s_playerInputResolver{nullptr};
    }

    void VehicleEntity::SetPlayerInputResolver(PlayerInputFn fn) {
        s_playerInputResolver.store(fn, std::memory_order_release);
    }

    bool VehicleEntity::GetPlayerInput(const Entity& player, PlayerInput& out) {
        const PlayerInputFn fn = s_playerInputResolver.load(std::memory_order_acquire);
        if (!fn || !player.IsPlayer()) return false;
        return fn(player, out);
    }

    glm::dvec3 VehicleEntity::GetLastClientMoveIntent(const Entity& player) {
        // MC ServerPlayer.getLastClientMoveIntent:
        //   left = left == right ? 0 : (left ? 1 : -1)
        //   forward = forward == backward ? 0 : (forward ? 1 : -1)
        //   getInputVector((left, 0, forward), 1, yRot)
        PlayerInput in;
        if (!GetPlayerInput(player, in)) return glm::dvec3(0.0);
        const double leftIntent = in.left == in.right ? 0.0 : (in.left ? 1.0 : -1.0);
        const double forwardIntent = in.forward == in.backward ? 0.0 : (in.forward ? 1.0 : -1.0);
        return Entity::GetInputVector(glm::dvec3(leftIntent, 0.0, forwardIntent), 1.0f, player.yRot);
    }

    // ── Construction ───────────────────────────────────────────────────────

    VehicleEntity::VehicleEntity(EntityTypeId type, EntityLevel* level)
        : Mob(type, level, NoAiTag{}) {
        // No health to speak of; the living attributes exist only because the
        // Mob pipeline (the wire's health field, the NBT's Health) reads them.
        CreateLivingAttributes(m_attributes);
        m_health = GetMaxHealth();
    }

    VehicleEntity::~VehicleEntity() = default;

    // ── Breaking ───────────────────────────────────────────────────────────

    bool VehicleEntity::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC VehicleEntity.hurtServer.
        if (!m_level || m_level->IsClientSide()) return true;   // hurtClient: true
        if (IsRemoved()) return true;

        const bool creativePlayer = attacker && attacker->IsPlayer() && attacker->IsCreative();
        // isInvulnerableToBase: the invulnerable flag (the void and a
        // creative player get through), fire against a fire-immune vehicle.
        if (IsInvulnerable() && source != MobDamageSource::Void && !creativePlayer) return false;
        if ((source == MobDamageSource::Fire || source == MobDamageSource::Lava) && FireImmune()) return false;

        // MC VehicleEntity.ignoreExplosion: an explosion whose indirect source
        // is a mob leaves vehicles alone while mob griefing is off. The
        // engine's non-mob entities ride the Mob class, so the cause is
        // resolved (a projectile's shooter, a TNT's igniter) and only a real
        // mob counts.
        if (source == MobDamageSource::Explosion && !m_level->MobGriefing() && attacker && !attacker->IsPlayer()) {
            Entity* cause = attacker;
            if (auto* projectile = dynamic_cast<Projectile*>(attacker)) cause = projectile->GetOwner();
            else if (auto* tnt = dynamic_cast<PrimedTnt*>(attacker))   cause = tnt->GetOwner();
            const bool causeIsMob = cause && !cause->IsPlayer() && dynamic_cast<Mob*>(cause) &&
                                    !dynamic_cast<Projectile*>(cause) && !dynamic_cast<PrimedTnt*>(cause) &&
                                    !dynamic_cast<FallingBlockEntity*>(cause) && !dynamic_cast<EndCrystal*>(cause) &&
                                    !dynamic_cast<BlockAttachedEntity*>(cause) && !dynamic_cast<ArmorStand*>(cause) &&
                                    !dynamic_cast<VehicleEntity*>(cause);
            if (causeIsMob) return false;
        }

        SetHurtDir(-GetHurtDir());
        SetHurtTime(10);
        hurtMarked = true;   // markHurt
        SetDamage(GetDamage() + amount * 10.0f);
        GameEvent(GameEventId::EntityDamage, attacker);

        const Entity* direct = HurtDirectEntity() ? HurtDirectEntity() : attacker;
        const bool sourceDestroys = ShouldSourceDestroy(source, direct);
        if ((creativePlayer || !(GetDamage() > 40.0f)) && !sourceDestroys) {
            if (creativePlayer) Discard();
        } else {
            Destroy(source, attacker);
        }
        return true;
    }

    void VehicleEntity::Destroy(MobDamageSource source, Entity* attacker) {
        (void)source; (void)attacker;
        DestroyWithItem(GetDropItem());
    }

    void VehicleEntity::KillVehicle() {
        // MC Entity.kill(level): remove(KILLED) and the ENTITY_DIE game
        // event. Remove ejects the passengers first.
        GameEvent(GameEventId::EntityDie);
        Remove(RemovalReason::Killed);
    }

    void VehicleEntity::DestroyWithItem(ItemID dropItem) {
        // MC VehicleEntity.destroy(level, item).
        KillVehicle();
        if (!m_level || m_level->IsClientSide() || !m_level->DoEntityDrops()) return;
        if (dropItem == Items::Air) return;
        ItemStack stack(dropItem, 1);
        if (const auto& name = GetCustomName()) stack.components.set(DataComponents::CUSTOM_NAME, *name);
        // Entity.spawnAtLocation(level, stack): at the feet, the default
        // pickup delay.
        m_level->SpawnItemStackDrop(position, stack);
    }

    void VehicleEntity::KillFromCommand() {
        // MC /kill → Entity.kill: no drop.
        KillVehicle();
    }

    void VehicleEntity::LavaHurt() {
        // MC Entity.lavaHurt: `if (!fireImmune()) { if (hurtServer(level,
        // damageSources().lava(), 4.0F)) playSound(GENERIC_BURN, ...) }`.
        if (FireImmune() || !m_level || m_level->IsClientSide()) return;
        if (Hurt(MobDamageSource::Lava, 4.0f, nullptr)) {
            JavaRandom& rng = m_level->Random();
            PlaySound("entity.generic.burn", 0.4f, 2.0f + rng.NextFloat() * 0.4f);
        }
    }

    void VehicleEntity::Knockback(double power, double dx, double dz) {
        // MC Player.attack on a non-living target: entity.push(-sin(yRot) *
        // knockback * 0.5, 0.1, cos(yRot) * knockback * 0.5). The engine's
        // callers pass the push direction as (dx, dz) pointing from the
        // victim toward the attacker, with `power` already the halved
        // strength.
        if (power <= 0.0) return;
        const double len = std::sqrt(dx * dx + dz * dz);
        if (len < 1.0e-9) return;
        PushBy(-dx / len * power, 0.1, -dz / len * power);
        hurtMarked = true;
    }

    void VehicleEntity::PushEntity(Entity& other) {
        EntityPushPair(*this, other);
    }

    namespace {
        // MC Entity.push(x, y, z) on any entity: a vehicle's own push, a
        // mob's velocity; a player's view is moved by its own client.
        void PushVelocity(Entity& e, double x, double z) {
            if (e.IsPlayer()) return;
            if (IsVehicleEntityType(e.GetType())) {
                static_cast<VehicleEntity&>(e).PushBy(x, 0.0, z);
                return;
            }
            e.velocity.x += x;
            e.velocity.z += z;
            e.needsSync = true;
            e.physicsParked = false;
        }
    } // namespace

    void VehicleEntity::EntityPushPair(Entity& self, Entity& other) {
        // MC Entity.push(Entity) with `self` as this.
        const auto rootVehicle = [](const Entity* e) {
            while (e->GetVehicle()) e = e->GetVehicle();
            return e;
        };
        if (rootVehicle(&other) == rootVehicle(&self)) return;   // isPassengerOfSameVehicle
        if (other.IsSpectator() || self.IsSpectator()) return;   // noPhysics
        double xa = other.position.x - self.position.x;
        double za = other.position.z - self.position.z;
        double dd = std::max(std::abs(xa), std::abs(za));
        if (dd < 0.009999999776482582) return;
        dd = std::sqrt(dd);
        xa /= dd;
        za /= dd;
        double pow = 1.0 / dd;
        if (pow > 1.0) pow = 1.0;
        xa *= pow;
        za *= pow;
        xa *= 0.05000000074505806;
        za *= 0.05000000074505806;
        if (!self.IsVehicle() && self.IsPushable()) PushVelocity(self, -xa, -za);
        if (!other.IsVehicle() && other.IsPushable()) PushVelocity(other, xa, za);
    }

    void VehicleEntity::TickHurtAndDamage() {
        if (GetHurtTime() > 0) SetHurtTime(GetHurtTime() - 1);
        if (GetDamage() > 0.0f) SetDamage(GetDamage() - 1.0f);
    }

    void VehicleEntity::VehicleBaseTick() {
        // MC Entity.baseTick, in its order.
        if (IsPassenger() && GetVehicle()->IsRemoved()) StopRiding();
        if (m_boardingCooldown > 0) --m_boardingCooldown;
        // (handlePortal is the server's TickPortals; no sprint dust.)
        UpdateInWaterStateAndDoFluidPushing();   // + LavaFluid.entityInside's ignite and hurt
        if (m_level && !m_level->IsClientSide() && m_remainingFireTicks > 0) {
            if (FireImmune()) {
                m_remainingFireTicks = std::max(0, m_remainingFireTicks - 4);
            } else {
                // A non-living entity takes on_fire damage every second it
                // burns outside lava — a boat burns apart in about four.
                if (m_remainingFireTicks % 20 == 0 && !IsInLava()) {
                    Hurt(MobDamageSource::Fire, 1.0f, nullptr);
                }
                --m_remainingFireTicks;
            }
        }
        if (IsOnFire() && IsInWater()) {
            ClearFire();
            PlayEntityOnFireExtinguishedSound();
        }
        if (IsInLava()) fallDistance *= 0.5f;
        // checkBelowWorld.
        if (m_level && position.y < static_cast<double>(m_level->GetMinY() - 64)) Discard();
        firstTick = false;
        if (m_level && !m_level->IsClientSide()) TickLeash();
    }

    // ── Moving ─────────────────────────────────────────────────────────────

    bool VehicleEntity::CanVehicleCollideWith(const Entity& other) const {
        if (&other == this || other.IsRemoved() || other.IsSpectator()) return false;
        // isPassengerOfSameVehicle.
        const auto rootVehicle = [](const Entity* e) {
            while (e->GetVehicle()) e = e->GetVehicle();
            return e;
        };
        if (rootVehicle(&other) == rootVehicle(this)) return false;
        bool solid = false;
        if (IsVehicleEntityType(other.GetType())) {
            solid = static_cast<const VehicleEntity&>(other).CanBeCollidedWith();
        }
        return solid || other.IsPushable();
    }

    void VehicleEntity::CollectEntityColliders(const AABBd& region, std::vector<AABBd>& out) const {
        out.clear();
        if (!m_level) return;
        const AABB query = AABB::FromMinMax(glm::vec3(region.min - glm::dvec3(1.0e-7)),
                                            glm::vec3(region.max + glm::dvec3(1.0e-7)));
        thread_local std::vector<Entity*> t_near;
        t_near.clear();
        m_level->GetEntitiesInBox(query, this, t_near);
        for (const Entity* e : t_near) {
            if (!e || !CanVehicleCollideWith(*e)) continue;
            const AABBd box = e->GetAABBd();
            if (box.Intersects(region)) out.push_back(box);
        }
    }

    bool VehicleEntity::NoCollision(const AABBd& box) const {
        if (!m_level) return true;
        PhysicsContext context = m_level->Physics();
        if (CollidesAt(box, context)) return false;
        thread_local std::vector<AABBd> t_boxes;
        CollectEntityColliders(box, t_boxes);
        for (const AABBd& b : t_boxes) {
            if (b.Intersects(box)) return false;
        }
        return true;
    }

    float VehicleEntity::GetBlockSpeedFactor() const {
        // MC Entity.getBlockSpeedFactor.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 1.0f;
        const auto factorOf = [](BlockID id) {
            return (id == BlockID::SoulSand || id == BlockID::HoneyBlock) ? 0.4f : 1.0f;
        };
        const glm::ivec3 p = BlockPosition();
        const BlockID here = blocks->GetBlock(p.x, p.y, p.z);
        const float speedFactor = factorOf(here);
        if (here == BlockID::Water || here == BlockID::BubbleColumn) return speedFactor;
        if (speedFactor != 1.0f) return speedFactor;
        const int belowY = static_cast<int>(std::floor(position.y - 0.500001));
        return factorOf(blocks->GetBlock(p.x, belowY, p.z));
    }

    void VehicleEntity::MoveVehicle(const glm::dvec3& delta) {
        if (!m_level || IsPassenger()) return;
        // A move is a step, never a teleport (see Entity::Move).
        if (!(glm::dot(delta, delta) <= 64.0 * 64.0)) {
            velocity = glm::dvec3(0.0);
            return;
        }
        const glm::dvec3 currentMovement = velocity;   // getDeltaMovement() before the move

        // The region the move can reach, for the entity boxes (MC
        // getEntityCollisions(this, box.expandTowards(movement))).
        AABBd swept = GetAABBd();
        swept.min += glm::dvec3(std::min(0.0, delta.x), std::min(0.0, delta.y), std::min(0.0, delta.z));
        swept.max += glm::dvec3(std::max(0.0, delta.x), std::max(0.0, delta.y), std::max(0.0, delta.z));
        thread_local std::vector<AABBd> t_entityBoxes;
        CollectEntityColliders(swept, t_entityBoxes);

        PhysicsContext context = m_level->Physics();
        context.collisionEntity       = true;
        context.collisionFallDistance = fallDistance;
        context.entityColliders       = t_entityBoxes.empty() ? nullptr : &t_entityBoxes;

        const glm::dvec3 start = position;
        glm::dvec3 moveVel = delta;
        const EntityMoveResult result = MoveEntity(position, moveVel, HalfExtents(), 0.0f, onGround, context);
        (void)result;
        const glm::dvec3 movement = position - start;

        // Mth.equal: |a - b| < 1e-5.
        const bool xCollision = std::abs(delta.x - movement.x) >= 1.0e-5;
        const bool zCollision = std::abs(delta.z - movement.z) >= 1.0e-5;
        horizontalCollision = xCollision || zCollision;
        const bool movedVertically = std::abs(delta.y) > 0.0;
        if (movedVertically || IsLocalInstanceAuthoritative()) {
            verticalCollision = std::abs(delta.y - movement.y) >= 1.0e-9;
            const bool below = verticalCollision && delta.y < 0.0;
            onGround = below;
        }

        const IBlockAccess* blocks = m_level->Blocks();
        // getOnPosLegacy — the block under the feet, 0.2 down.
        const glm::ivec3 effectPos(static_cast<int>(std::floor(position.x)),
                                   static_cast<int>(std::floor(position.y - 0.2)),
                                   static_cast<int>(std::floor(position.z)));
        const BlockID effectBlock = blocks ? blocks->GetBlock(effectPos.x, effectPos.y, effectPos.z) : BlockID::Air;
        if (IsLocalInstanceAuthoritative()) VehicleCheckFallDamage(movement.y, onGround);
        if (IsRemoved()) return;

        // restituteMovementAfterCollisions (26.3): a non-living entity's
        // entity bounciness is 0 and its block bounciness 0.8 of the block's
        // — a boat dropped on slime bounces at 0.8.
        glm::dvec3 after = currentMovement;
        if (IsLocalInstanceAuthoritative() && ((movedVertically && verticalCollision) || horizontalCollision)) {
            double restitution = 0.0;
            if (xCollision) after.x = -currentMovement.x * restitution;
            if (zCollision) after.z = -currentMovement.z * restitution;
            if (verticalCollision) {
                const bool below = delta.y < 0.0;
                if (below) {
                    const double gravity = GetGravity();
                    restitution = (!(-currentMovement.y <= gravity) && !SuppressesBounce(effectBlock))
                        ? std::max(restitution, static_cast<double>(BounceRestitution(effectBlock)) * 0.8)
                        : 0.0;
                }
                double gravityCompensation = 0.0, effectiveDrag = 1.0;
                if (restitution > 0.0 && std::abs(currentMovement.y) > 1.0e-12) {
                    const double portionWithMovement = movement.y / currentMovement.y;
                    gravityCompensation = portionWithMovement * GetGravity();
                    effectiveDrag = 1.0 + portionWithMovement * (static_cast<double>(GetAirDrag()) - 1.0);
                }
                after.y = (gravityCompensation - currentMovement.y) * effectiveDrag * restitution;
                if (restitution > 0.0) {
                    GameEvent(GameEventId::Bounce);
                    needsSync = true;   // syncPosition
                }
            }
            velocity = after;
        }

        // applyMovementEmissionAndPlaySound: a boat's or cart's movement is
        // felt, not heard (MovementEmission.EVENTS) — SWIM in water, STEP on
        // the ground, for a sculk sensor. Server side (the driver's accepted
        // move included).
        if (!m_level->IsClientSide() && (EmitsMovementSounds() || EmitsMovementEvents())) {
            ApplyMovementEmissionAndPlaySound(movement);
        }

        // getBlockSpeedFactor.
        const float factor = GetBlockSpeedFactor();
        if (factor != 1.0f) {
            velocity.x *= factor;
            velocity.z *= factor;
        }
        physicsParked = false;
    }

    // ── Passengers ─────────────────────────────────────────────────────────

    int VehicleEntity::PassengerSlot(const Entity& passenger) const {
        if (m_level && m_level->IsClientSide()) return PassengerSlotOfId(passenger.GetId());
        const auto& list = GetPassengers();
        for (size_t i = 0; i < list.size(); ++i) if (list[i] == &passenger) return static_cast<int>(i);
        return 0;
    }

    int VehicleEntity::PassengerSlotOfId(int32_t id) const {
        if (m_level && m_level->IsClientSide() && !m_syncedPassengers.empty()) {
            for (size_t i = 0; i < m_syncedPassengers.size(); ++i) {
                if (m_syncedPassengers[i] == id) return static_cast<int>(i);
            }
            return 0;
        }
        const auto& list = GetPassengers();
        for (size_t i = 0; i < list.size(); ++i) if (list[i] && list[i]->GetId() == id) return static_cast<int>(i);
        return 0;
    }

    int VehicleEntity::PassengerTotal() const {
        if (m_level && m_level->IsClientSide()) {
            const int synced = static_cast<int>(m_syncedPassengers.size());
            // The list the server sent, or (before it arrives) the mobs the
            // client has linked on its own.
            return std::max(synced, static_cast<int>(GetPassengers().size()));
        }
        return static_cast<int>(GetPassengers().size());
    }

    bool VehicleEntity::IsPlayerPassengerId(int32_t id) const {
        // Player entity ids live below the item range (Game::kItemEntityIdBase).
        return id > 0 && id < kItemEntityIdBase;
    }

    bool VehicleEntity::FirstPassengerIsPlayer() const {
        if (m_level && m_level->IsClientSide()) {
            return !m_syncedPassengers.empty() && IsPlayerPassengerId(m_syncedPassengers.front());
        }
        const auto& list = GetPassengers();
        return !list.empty() && list.front() && list.front()->IsPlayer();
    }

    bool VehicleEntity::HasPlayerPassenger() const {
        if (m_level && m_level->IsClientSide()) {
            for (int32_t id : m_syncedPassengers) if (IsPlayerPassengerId(id)) return true;
            return false;
        }
        for (const Entity* p : GetPassengers()) if (p && p->IsPlayer()) return true;
        return false;
    }

    bool VehicleEntity::HasExactlyOnePlayerPassenger() const {
        int players = 0;
        if (m_level && m_level->IsClientSide()) {
            for (int32_t id : m_syncedPassengers) if (IsPlayerPassengerId(id)) ++players;
            return players == 1;
        }
        // MC counts the whole indirect passenger tree.
        std::vector<const Entity*> stack(GetPassengers().begin(), GetPassengers().end());
        while (!stack.empty()) {
            const Entity* e = stack.back();
            stack.pop_back();
            if (!e) continue;
            if (e->IsPlayer()) ++players;
            for (const Entity* p : e->GetPassengers()) stack.push_back(p);
        }
        return players == 1;
    }

    void VehicleEntity::SetSyncedPassengers(const std::vector<int32_t>& ids, int32_t localPlayerId) {
        m_syncedPassengers = ids;
        m_localPlayerId = localPlayerId;
    }

    void VehicleEntity::CollectPassengerWireIds(std::vector<int32_t>& out,
                                                int32_t (*playerIdOf)(const Entity&)) const {
        out.clear();
        for (const Entity* p : GetPassengers()) {
            if (!p) continue;
            if (p->IsPlayer()) {
                const int32_t id = playerIdOf ? playerIdOf(*p) : p->GetId();
                if (id > 0) out.push_back(id);
            } else {
                out.push_back(p->GetId());
            }
        }
    }

    bool VehicleEntity::ConsumePassengersDirty(int32_t (*playerIdOf)(const Entity&)) const {
        std::vector<int32_t> now;
        CollectPassengerWireIds(now, playerIdOf);
        if (m_passengersSentOnce && now == m_lastSentPassengers) return false;
        // An empty list the client already assumes needs no packet on first sight.
        const bool changed = m_passengersSentOnce || !now.empty();
        m_lastSentPassengers = std::move(now);
        m_passengersSentOnce = true;
        return changed;
    }

    Entity* VehicleEntity::GetControllingPassenger() const {
        if (!IsControlledByPlayer()) return nullptr;
        if (m_level && m_level->IsClientSide()) return nullptr;   // players are no client entities
        const auto& list = GetPassengers();
        return list.empty() ? nullptr : list.front();
    }

    bool VehicleEntity::IsLocalInstanceAuthoritative() const {
        if (!m_level) return true;
        if (m_level->IsClientSide()) return m_localControlled && IsControlledByPlayer();
        return !IsControlledByPlayer();
    }

    glm::dvec3 VehicleEntity::GetPassengerAttachmentPoint(const Entity& passenger) const {
        const bool animal = dynamic_cast<const Animal*>(&passenger) != nullptr;
        return PassengerAttachmentAt(PassengerSlot(passenger), std::max(1, PassengerTotal()), animal, yRot);
    }

    glm::dvec3 VehicleEntity::PlayerSeatFeet(int slot, int total, float playerScale) const {
        return PlayerSeatFeetAt(position, yRot, slot, total, playerScale);
    }

    glm::dvec3 VehicleEntity::PlayerSeatFeetAt(const glm::dvec3& vehiclePos, float vehicleYRot, int slot,
                                               int total, float playerScale) const {
        // positionRider: getPassengerRidingPosition(player) minus the
        // player's VEHICLE attachment (0, 0.6, 0) at its scale.
        const glm::dvec3 attach = PassengerAttachmentAt(slot, std::max(1, total), false, vehicleYRot);
        return vehiclePos + attach - glm::dvec3(0.0, 0.6 * static_cast<double>(playerScale), 0.0);
    }

    bool VehicleEntity::CanAddPassenger(const Entity& passenger) const {
        (void)passenger;
        return static_cast<int>(GetPassengers().size()) < GetMaxPassengers();
    }

    // ── Wire ───────────────────────────────────────────────────────────────

    void VehicleEntity::FillSyncedData(VehicleSyncedData& out) const {
        out.hurtTime = m_hurtTime;
        out.hurtDir  = m_hurtDir;
        out.damage   = m_damage;
    }

    void VehicleEntity::ApplySyncedData(const VehicleSyncedData& in) {
        m_hurtTime = in.hurtTime;
        m_hurtDir  = in.hurtDir;
        m_damage   = in.damage;
    }

    bool VehicleEntity::ConsumeDataDirty() const {
        VehicleSyncedData now;
        FillSyncedData(now);
        if (m_dataSentOnce && now == m_lastSentData) return false;
        m_lastSentData = now;
        m_dataSentOnce = true;
        return true;
    }

    // ── Dismount helpers ───────────────────────────────────────────────────

    namespace {
        // The collision shape's top in its cell, -inf for no shape.
        double ShapeMaxY(const IBlockAccess& level, const glm::ivec3& pos) {
            const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
            if (!BlockRegistry::HasCollision(state.Block())) return -std::numeric_limits<double>::infinity();
            const BlockRegistry::BlockShapeSet shape = BlockRegistry::GetBlockCollisionShapeSet(state);
            if (shape.count == 0) return -std::numeric_limits<double>::infinity();
            double top = -std::numeric_limits<double>::infinity();
            for (const auto& box : shape) {
                if (box.max.y <= box.min.y) continue;
                top = std::max(top, static_cast<double>(box.max.y));
            }
            return top;
        }
    } // namespace

    double VehicleEntity::BlockFloorHeight(const IBlockAccess& level, const glm::ivec3& pos) {
        // MC BlockGetter.getBlockFloorHeight(blockShape, belowShape).
        const double here = ShapeMaxY(level, pos);
        if (!std::isinf(here)) return here;
        const double below = ShapeMaxY(level, glm::ivec3(pos.x, pos.y - 1, pos.z));
        return below >= 1.0 ? below - 1.0 : -std::numeric_limits<double>::infinity();
    }

    bool VehicleEntity::CanDismountTo(const IBlockAccess& level, const AABBd& box) {
        PhysicsContext context;
        context.blockAccess = &level;
        return !CollidesAt(box, context);
    }

    void VehicleEntity::DismountPoseHeights(const LivingEntity& passenger, std::vector<double>& heights) {
        heights.clear();
        const double scale = static_cast<double>(passenger.scale);
        if (passenger.IsPlayer()) {
            // Player.getDismountPoses: STANDING, CROUCHING, SWIMMING
            // (Avatar.POSES: 1.8, 1.5, 0.6).
            heights.push_back(1.8 * scale);
            heights.push_back(1.5 * scale);
            heights.push_back(0.6 * scale);
        } else {
            heights.push_back(static_cast<double>(passenger.GetBbHeight()));
        }
    }

    double VehicleEntity::PassengerWidth(const LivingEntity& passenger) {
        if (passenger.IsPlayer()) return 0.6 * static_cast<double>(passenger.scale);
        return static_cast<double>(passenger.GetBbWidth());
    }

    glm::dvec3 VehicleEntity::CollisionHorizontalEscapeVector(double colliderWidth, double collidingWidth,
                                                              float directionDegrees) {
        // MC Entity.getCollisionHorizontalEscapeVector.
        const double distance = (colliderWidth + collidingWidth + 9.999999747378752E-6) / 2.0;
        const float directionX = -std::sin(directionDegrees * Mth::kDegToRad);
        const float directionZ = std::cos(directionDegrees * Mth::kDegToRad);
        const float scale = std::max(std::abs(directionX), std::abs(directionZ));
        return glm::dvec3(static_cast<double>(directionX) * distance / static_cast<double>(scale), 0.0,
                          static_cast<double>(directionZ) * distance / static_cast<double>(scale));
    }

    glm::dvec3 PlayerSeatFeetOn(const Entity& vehicle, const Entity* passenger, int32_t passengerId,
                                float playerScale) {
        if (IsVehicleEntityType(vehicle.GetType())) {
            const auto& v = static_cast<const VehicleEntity&>(vehicle);
            const int slot = passenger ? v.PassengerSlot(*passenger) : v.PassengerSlotOfId(passengerId);
            return v.PlayerSeatFeet(slot, std::max(1, v.PassengerTotal()), playerScale);
        }
        if (const auto* seat = dynamic_cast<const PlayerRideable*>(&vehicle)) {
            return seat->PlayerRiderPosition();
        }
        // Entity.positionRider: getPassengerRidingPosition(player) minus the
        // player's vehicle attachment — the seat known by its place: the
        // passenger list on the server, the synched order on a client.
        const glm::dvec3 feetOffset(0.0, 0.6 * static_cast<double>(playerScale), 0.0);
        if (passenger) return vehicle.position + vehicle.GetPassengerAttachmentPoint(*passenger) - feetOffset;
        int slot = 0, total = 1;
        if (const auto* living = dynamic_cast<const LivingEntity*>(&vehicle);
            living && !living->SyncedRiders().empty()) {
            const auto& ids = living->SyncedRiders();
            total = static_cast<int>(ids.size());
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == passengerId) { slot = static_cast<int>(i); break; }
            }
        }
        return vehicle.position + vehicle.GetPassengerAttachmentForSlot(slot, total) - feetOffset;
    }

} // namespace Game
