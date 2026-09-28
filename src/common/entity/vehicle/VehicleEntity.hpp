// File: src/common/entity/vehicle/VehicleEntity.hpp
//
// MC net.minecraft.world.entity.vehicle.VehicleEntity — what boats and
// minecarts share: the synched hurt shake (DATA_ID_HURT / HURTDIR / DAMAGE),
// breaking into the vehicle's item once 40 damage has piled up faster than
// it decays, and the passenger machinery a vehicle needs on top of Entity's.
//
// ── Why a Mob ─────────────────────────────────────────────────────────────
// MC's vehicles are plain Entities; here they ride the Mob pipeline (Mob's
// NoAiTag, like the end crystal and the armor stand) because the server's
// tracker, the wire, the NBT and the client's entity store are Mob-shaped.
// Tick replaces Mob::Tick wholesale; nothing of the living machinery runs.
//
// ── Players as passengers ─────────────────────────────────────────────────
// A player is no Entity here, but its server-side PlayerEntityView is: the
// riding system (Server::PlayerRiding) puts that view in m_passengers, so
// the boat code below reads MC's passenger list as MC does — the first
// passenger is the controlling one, two seats, a player first. On the CLIENT
// the players are not entities at all, so the vehicle keeps the passenger
// order the server sent (SetPassengersS2C, player ids included) and answers
// the seat questions from it (PassengerSlot / PassengerTotal).
//
// ── Who simulates ──────────────────────────────────────────────────────────
// MC Entity.isLocalInstanceAuthoritative: a vehicle driven by a player is
// simulated by that player's CLIENT (the boat), which reports where it went
// (ServerboundMoveVehiclePacket); everything else by the server. Minecarts
// never have a controlling passenger, so they are always the server's.
#pragma once

#include "common/entity/Mob.hpp"
#include "common/entity/Item.hpp"
#include "common/inventory/MenuType.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Game {

    class LivingEntity;
    class ILevelWrite;
    class VehicleContainer;
    struct IBlockAccess;

    // ── The vehicle entity types ────────────────────────────────────────────
    bool IsBoatEntityType(EntityTypeId type);        // every boat and raft, chest or not
    bool IsChestBoatEntityType(EntityTypeId type);   // the chest boats and the chest raft
    bool IsRaftEntityType(EntityTypeId type);        // bamboo raft / chest raft
    bool IsMinecartEntityType(EntityTypeId type);    // all seven minecarts
    inline bool IsVehicleEntityType(EntityTypeId type) {
        return IsBoatEntityType(type) || IsMinecartEntityType(type);
    }
    // The boat entity an item places (and the item a boat drops), Count /
    // Air when the item is no boat.
    EntityTypeId BoatTypeForItem(ItemID item);
    ItemID       BoatItemForType(EntityTypeId type);
    // The minecart an item places (MinecartItem), Count when none.
    EntityTypeId MinecartTypeForItem(ItemID item);

    // MC net.minecraft.world.entity.player.Input — the movement keys a
    // client reports (ServerboundPlayerInputPacket). The server keeps the
    // last one per player (ServerPlayer.lastClientInput): a minecart reads
    // its move intent, the riding tick its shift.
    struct PlayerInput {
        bool forward  = false;
        bool backward = false;
        bool left     = false;
        bool right    = false;
        bool jump     = false;
        bool shift    = false;
        bool sprint   = false;

        uint8_t Pack() const {
            return static_cast<uint8_t>((forward ? 0x01 : 0) | (backward ? 0x02 : 0) | (left ? 0x04 : 0) |
                                        (right ? 0x08 : 0) | (jump ? 0x10 : 0) | (shift ? 0x20 : 0) |
                                        (sprint ? 0x40 : 0));
        }
        static PlayerInput Unpack(uint8_t b) {
            PlayerInput in;
            in.forward = (b & 0x01) != 0; in.backward = (b & 0x02) != 0;
            in.left    = (b & 0x04) != 0; in.right    = (b & 0x08) != 0;
            in.jump    = (b & 0x10) != 0; in.shift    = (b & 0x20) != 0;
            in.sprint  = (b & 0x40) != 0;
            return in;
        }
        bool operator==(const PlayerInput& o) const { return Pack() == o.Pack(); }
        bool operator!=(const PlayerInput& o) const { return Pack() != o.Pack(); }
    };

    // Everything a vehicle synchs beyond the Mob wire (VehicleDataS2C): MC's
    // VehicleEntity, AbstractBoat and AbstractMinecart / MinecartFurnace
    // entity data accessors, in one record.
    struct VehicleSyncedData {
        int32_t hurtTime = 0;        // DATA_ID_HURT
        int32_t hurtDir  = 1;        // DATA_ID_HURTDIR
        float   damage   = 0.0f;     // DATA_ID_DAMAGE
        // AbstractBoat.
        bool    paddleLeft  = false; // DATA_ID_PADDLE_LEFT
        bool    paddleRight = false; // DATA_ID_PADDLE_RIGHT
        // The engine's back-paddling (a deliberate addition): the paddles
        // row in reverse while the boat is driven backward.
        bool    paddleReverse = false;
        int32_t bubbleTime  = 0;     // DATA_ID_BUBBLE_TIME
        // AbstractMinecart.
        bool     hasCustomDisplay = false;   // DATA_ID_CUSTOM_DISPLAY_BLOCK present
        uint32_t customDisplay    = 0;       //   its BlockState raw id
        int32_t  displayOffset    = 0;       // DATA_ID_DISPLAY_OFFSET
        // MinecartFurnace.
        bool     hasFuel          = false;   // DATA_ID_FUEL

        bool operator==(const VehicleSyncedData& o) const {
            return hurtTime == o.hurtTime && hurtDir == o.hurtDir && damage == o.damage &&
                   paddleLeft == o.paddleLeft && paddleRight == o.paddleRight &&
                   paddleReverse == o.paddleReverse && bubbleTime == o.bubbleTime &&
                   hasCustomDisplay == o.hasCustomDisplay && customDisplay == o.customDisplay &&
                   displayOffset == o.displayOffset && hasFuel == o.hasFuel;
        }
        bool operator!=(const VehicleSyncedData& o) const { return !(*this == o); }
    };

    // Where a PLAYER's feet go on any rideable entity — a boat's or a
    // minecart's seat by its place in the passenger list, an equine's or a
    // cushion's own seat (PlayerRideable), else MC's generic positionRider
    // (the vehicle's passenger attachment less the player's 0.6). `passenger`
    // is the player's entity where there is one (the server's view), else
    // `passengerId` names it in the vehicle's synched list (a client).
    glm::dvec3 PlayerSeatFeetOn(const Entity& vehicle, const Entity* passenger, int32_t passengerId,
                                float playerScale);

    class VehicleEntity : public Mob {
    public:
        ~VehicleEntity() override;

        // ── MC VehicleEntity synched data ──────────────────────────────────
        int   GetHurtTime() const { return m_hurtTime; }
        void  SetHurtTime(int t) { m_hurtTime = t; }
        int   GetHurtDir() const { return m_hurtDir; }
        void  SetHurtDir(int d) { m_hurtDir = d; }
        float GetDamage() const { return m_damage; }
        void  SetDamage(float d) { m_damage = d; }

        // ── Breaking (MC VehicleEntity.hurtServer / destroy) ──────────────
        // Every hit adds damage × 10 and shakes the vehicle; past 40 (or at
        // once for a creative player, who discards it without a drop) it
        // breaks into its item. Lightning, fire and lava go through here too.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC shouldSourceDestroy — the TNT minecart takes fire and blasts as
        // a destroying hit.
        virtual bool ShouldSourceDestroy(MobDamageSource source, const Entity* direct) const {
            (void)source; (void)direct;
            return false;
        }
        // MC destroy(level, source) — the drop (and a chest vehicle's
        // contents). Server only.
        virtual void Destroy(MobDamageSource source, Entity* attacker);
        // MC destroy(level, item): killed, and the item (carrying this
        // vehicle's custom name) dropped when entity drops are on.
        void DestroyWithItem(ItemID dropItem);
        // MC getDropItem / getPickResult.
        virtual ItemID    GetDropItem() const = 0;
        virtual ItemStack GetPickResult() const { return ItemStack(GetDropItem(), 1); }
        // MC kill(level): removed as KILLED, passengers ejected first.
        void KillVehicle();

        // MC VehicleEntity.getDimensionChangingDelay.
        int GetDimensionChangingDelay() const override { return 10; }

        // ── Not a living thing ────────────────────────────────────────────
        bool IsPickable() const override { return !IsRemoved(); }
        bool IsAttackable() const override { return true; }
        bool IsAffectedByPotions() const override { return false; }
        bool IsAlive() const override { return !IsRemoved(); }
        void CheckDespawn() override {}
        bool RemoveWhenFarAway(double) const override { return false; }
        bool RequiresCustomPersistence() const override { return true; }
        float MaxUpStep() const override { return 0.0f; }
        // MC Entity.causeFallDamage: nothing for the vehicle itself (its
        // riders take the fall — Entity::PropagateFallToPassengers).
        bool CauseFallDamage(double fallDist, float damageMultiplier) override {
            (void)fallDist; (void)damageMultiplier;
            return false;
        }
        // Entity's checkFallDamage, not LivingEntity's (no landing dust).
        void CheckFallDamage(double dy, bool onGroundNow) override { Entity::CheckFallDamage(dy, onGroundNow); }
        // A player's knockback on a non-living target is MC Entity.push.
        void Knockback(double power, double dx, double dz) override;
        void KillFromCommand() override;
        // MC Entity.lavaHurt: 4 fire damage (a boat in lava burns away).
        void LavaHurt() override;

        // MC Entity.push(Entity): the shove two overlapping entities share.
        // Boats and minecarts override it with their own rules.
        virtual void PushEntity(Entity& other);
        // MC Entity.push(Entity) between any two entities, `self` being MC's
        // `this`: both shoved apart by up to 0.05 a tick (a vehicle carrying
        // someone, or an unpushable party, stays put).
        static void EntityPushPair(Entity& self, Entity& other);
        // MC Entity.push(x, y, z).
        void PushBy(double x, double y, double z) {
            velocity += glm::dvec3(x, y, z);
            needsSync = true;
            physicsParked = false;
        }

        // ── Passengers, players included ───────────────────────────────────
        // The place of `passenger` in the passenger list and the list's
        // length — m_passengers on the server (the player views are in it),
        // the server's synched order on the client.
        int  PassengerSlot(const Entity& passenger) const;
        int  PassengerSlotOfId(int32_t id) const;
        int  PassengerTotal() const;
        bool IsPlayerPassengerId(int32_t id) const;
        // MC getFirstPassenger() instanceof Player.
        bool FirstPassengerIsPlayer() const;
        // Any player aboard (a client learns the players from the synched list).
        bool HasPlayerPassenger() const;
        // MC Entity.hasExactlyOnePlayerPassenger.
        bool HasExactlyOnePlayerPassenger() const;
        // The client's copy of the server's passenger list (entity ids; a
        // player's is its player id), and which of them is this client's own
        // player (0 = none).
        void SetSyncedPassengers(const std::vector<int32_t>& ids, int32_t localPlayerId);
        const std::vector<int32_t>& SyncedPassengers() const { return m_syncedPassengers; }
        // The ids the server sends in SetPassengersS2C — each passenger's
        // entity id, a player's view by its player id (resolved through
        // `playerIdOf`, which answers -1 for a view whose player is gone).
        void CollectPassengerWireIds(std::vector<int32_t>& out, int32_t (*playerIdOf)(const Entity&)) const;
        // Server: the passenger list changed since the last SetPassengersS2C.
        bool ConsumePassengersDirty(int32_t (*playerIdOf)(const Entity&)) const;

        // MC Entity.getControllingPassenger — the first passenger when it is
        // a player (server: its view). Null for minecarts.
        Entity* GetControllingPassenger() const override;
        // Does a player drive this (the boat's first seat)? Server: the
        // first passenger is a player's view; client: the first synched id
        // is a player.
        virtual bool IsControlledByPlayer() const { return false; }
        // Client: this client's own player is the one driving.
        bool IsControlledByLocalPlayer() const { return m_localControlled; }
        // MC Entity.isLocalInstanceAuthoritative for a vehicle: the server
        // simulates it unless a player drives it; a client only the vehicle
        // its own player drives.
        bool IsLocalInstanceAuthoritative() const;

        // Where a player in `slot` of `total` sits — MC positionRider for a
        // player: the seat (getPassengerRidingPosition) minus the player's
        // vehicle attachment (Avatar.DEFAULT_VEHICLE_ATTACHMENT, 0.6 up,
        // scaled). The client places its own and the remote players with it.
        glm::dvec3 PlayerSeatFeet(int slot, int total, float playerScale) const;
        glm::dvec3 PlayerSeatFeetAt(const glm::dvec3& vehiclePos, float vehicleYRot, int slot, int total,
                                    float playerScale) const;
        // The attachment of passenger `slot` of `total` relative to the
        // vehicle's feet, already turned by `vehicleYRot` (MC's
        // getPassengerAttachmentPoint for a passenger known by its place).
        virtual glm::dvec3 PassengerAttachmentAt(int slot, int total, bool animalPassenger,
                                                 float vehicleYRot) const = 0;
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;

        // MC VehicleEntity/AbstractBoat.canAddPassenger.
        bool CanAddPassenger(const Entity& passenger) const override;
        virtual int GetMaxPassengers() const { return 1; }

        // MC level.noCollision(this, getBoundingBox()) where it stands — the
        // placing item's last test.
        bool NoCollisionHere() const { return NoCollision(GetAABBd()); }

        // ── Interaction (MC interact, after Entity.interact's leash half) ──
        // `sneaking` is the player's isSecondaryUseActive. Server-side
        // effects (a ride, a menu) go through the level.
        virtual UseResult VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) {
            (void)player; (void)held; (void)sneaking;
            return UseResult::Pass;
        }

        // ── MC ContainerEntity ─────────────────────────────────────────────
        // The storage of a chest boat / chest raft / chest or hopper
        // minecart; null for the rest.
        virtual VehicleContainer* GetVehicleContainer() { return nullptr; }
        // The menu over it (MenuType::Generic9x3 or Hopper) and its title.
        virtual MenuType GetContainerMenuType() const { return MenuType::Generic9x3; }
        // MC Container.stopOpen: a player's menu over this closed —
        // CONTAINER_CLOSE for the chest boats and the chest minecart.
        virtual void StopOpenContainer(LivingEntity& player) { (void)player; }

        // ── The riding player's keys ───────────────────────────────────────
        // The server's store of each player's last PlayerInput (Server::
        // PlayerRiding installs it): a minecart's move intent, an equine's
        // steering.
        using PlayerInputFn = bool (*)(const Entity& player, PlayerInput& out);
        static void SetPlayerInputResolver(PlayerInputFn fn);
        static bool GetPlayerInput(const Entity& player, PlayerInput& out);
        // MC ServerPlayer.getLastClientMoveIntent: the keys as a unit-ish
        // world vector along the player's facing.
        static glm::dvec3 GetLastClientMoveIntent(const Entity& player);

        // ── Wire (VehicleDataS2C) ─────────────────────────────────────────
        virtual void FillSyncedData(VehicleSyncedData& out) const;
        virtual void ApplySyncedData(const VehicleSyncedData& in);
        // Server: the synched data changed since the last send.
        bool ConsumeDataDirty() const;

        // Client: the local player drives this vehicle (the boat whose first
        // seat is ours) — its position is simulated here and reported.
        void SetLocalControlled(bool controlled) { m_localControlled = controlled; }

        // The server's replay of a driver's reported move (MC
        // handleMoveVehicle → vehicle.move(PLAYER, delta)): collision only —
        // the driving client ran the physics.
        void ServerMoveFromDriver(const glm::dvec3& delta) { MoveVehicle(delta); }
        // MC setOnGroundWithMovement + doCheckFallDamage for that move.
        void ServerLandFromDriver(double dy, bool onGroundNow) {
            onGround = onGroundNow;
            VehicleCheckFallDamage(dy, onGroundNow);
        }

        // ── Shared helpers (MC DismountHelper, block floor heights) ────────
        // MC BlockGetter.getBlockFloorHeight(pos).
        static double BlockFloorHeight(const IBlockAccess& level, const glm::ivec3& pos);
        static bool   IsBlockFloorValid(double h) { return !std::isinf(h) && h < 1.0; }
        // MC DismountHelper.canDismountTo(level, passenger, box): no block
        // collision there (the world border is not modelled).
        static bool   CanDismountTo(const IBlockAccess& level, const AABBd& box);
        // The passenger's box heights per dismount pose (MC getDismountPoses
        // × getDimensions(pose)): a player tries standing, crouching and
        // swimming, a mob its own box only.
        static void   DismountPoseHeights(const LivingEntity& passenger, std::vector<double>& heights);
        static double PassengerWidth(const LivingEntity& passenger);

        // MC Entity.getCollisionHorizontalEscapeVector.
        static glm::dvec3 CollisionHorizontalEscapeVector(double colliderWidth, double collidingWidth,
                                                          float directionDegrees);

        // MC Entity.canBeCollidedWith(other): a boat is solid to what meets
        // it (a player can stand on one); nothing else here is.
        virtual bool CanBeCollidedWith() const { return false; }
        // MC AbstractBoat.canVehicleCollide(this, entity): what this
        // vehicle's own move is stopped by — a solid entity, or anything
        // pushable — but never its own passengers.
        bool CanVehicleCollideWith(const Entity& other) const;

    protected:
        VehicleEntity(EntityTypeId type, EntityLevel* level);

        // MC Entity.move(SELF, delta) for a vehicle: the block colliders plus
        // the entity boxes CanVehicleCollideWith accepts, then MC's tail —
        // the collision flags, checkFallDamage, the 26.3 bounce
        // (restituteMovementAfterCollisions) and the block speed factor.
        void MoveVehicle(const glm::dvec3& delta);
        // MC getAirDrag (boats 1.0, minecarts 0.95) — the bounce's drag.
        virtual float GetAirDrag() const { return 1.0f; }
        // MC Entity.getBlockSpeedFactor (soul sand, honey: 0.4).
        virtual float GetBlockSpeedFactor() const;
        // MC Entity.checkFallDamage as each vehicle overrides it (the boat's
        // resets on landing, never hurting anyone).
        virtual void VehicleCheckFallDamage(double dy, bool onGroundNow) { Entity::CheckFallDamage(dy, onGroundNow); }
        // MC EntityGetter.getEntityCollisions(this, region).
        void CollectEntityColliders(const AABBd& region, std::vector<AABBd>& out) const;
        // MC Level.noCollision(this, box): no block and no solid entity there.
        bool NoCollision(const AABBd& box) const;

        // The hurt / damage decay every vehicle tick starts with.
        void TickHurtAndDamage();
        // MC Entity.baseTick for a vehicle — the engine's Entity::BaseTick
        // plus the fire damage a non-living entity takes (1 a second while
        // burning out of lava). Boats run it from their tick.
        void VehicleBaseTick();

        int   m_hurtTime = 0;
        int   m_hurtDir  = 1;
        float m_damage   = 0.0f;

        // Client: the server's passenger order (ids, players by player id).
        std::vector<int32_t> m_syncedPassengers;
        int32_t              m_localPlayerId = 0;
        bool                 m_localControlled = false;

        // Server: what the tracker last sent (see ConsumeDataDirty /
        // ConsumePassengersDirty). Mutable for the same reason as
        // Entity::needsSync — the tracker holds entities const.
        mutable VehicleSyncedData    m_lastSentData;
        mutable bool                 m_dataSentOnce = false;
        mutable std::vector<int32_t> m_lastSentPassengers;
        mutable bool                 m_passengersSentOnce = false;
    };

} // namespace Game
