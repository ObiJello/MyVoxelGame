// File: src/common/entity/vehicle/Minecart.hpp
//
// MC net.minecraft.world.entity.vehicle.minecart — AbstractMinecart with its
// DEFAULT movement, OldMinecartBehavior (26.3 swaps in NewMinecartBehavior
// only under the minecart_improvements experiment, which this engine does not
// enable), and the seven carts:
//
//   Minecart              rideable (players and mobs); an activator rail
//                         throws its rider off and shakes it
//   MinecartChest         27 slots (AbstractMinecartContainer)
//   MinecartHopper        5 slots; sucks items from the container above and
//                         from the ground; an activator rail locks it
//   MinecartFurnace       burns coal / charcoal for 3600 ticks a lump (at
//                         most 32000), pushes along its heading
//   MinecartTNT           primed by an activator rail, fire, a flaming arrow,
//                         a blast, a fall of 3+ or a crash at speed; the blast
//                         grows with speed (4 + 1.5 × random × speed)
//   MinecartSpawner       a monster spawner on wheels (commands only)
//   MinecartCommandBlock  runs its command on a powered activator rail
//
// THE TRACK (OldMinecartBehavior.moveAlongTrack): snapped onto the rail's
// centreline between its two exits, 1/128 a tick of slope slide, speed along
// the rail kept, capped at 0.4 (0.2 in water) per tick — 8 blocks a second;
// a powered rail adds 0.06 a tick in the direction of travel (or kicks a
// cart at rest off the solid block at its end), an unpowered one brakes by
// half and stops it under 0.03; the slowdown is 0.997 with a rider, 0.96
// without (0.98 + emptiness/1000 for a container cart), 0.95 in water.
//
// The server simulates every minecart; clients interpolate (its rendering
// snaps the body onto the rail from the position — MinecartRenderer's
// oldRender).
#pragma once

#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/entity/vehicle/VehicleContainer.hpp"
#include "common/world/block/BlockState.hpp"

#include <memory>
#include <optional>
#include <string>

namespace Game {

    enum class RailShape : uint8_t;
    class SpawnerBlockEntity;

    class AbstractMinecart : public VehicleEntity {
    public:
        ~AbstractMinecart() override;

        // ── MC constants ───────────────────────────────────────────────────
        static constexpr float kWaterSlowdownFactor = 0.95f;   // WATER_SLOWDOWN_FACTOR
        // OldMinecartBehavior.
        static constexpr double kMaxSpeedInWater = 0.2;
        static constexpr double kMaxSpeedOnLand  = 0.4;

        // MC setInitialPos.
        void SetInitialPos(const glm::dvec3& pos) { position = pos; oldPosition = pos; }

        // ── Entity hooks ───────────────────────────────────────────────────
        void Tick() override;
        glm::dvec3 PassengerAttachmentAt(int slot, int total, bool animalPassenger,
                                         float vehicleYRot) const override;
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;
        glm::dvec3 GetDismountLocationForPassenger(const LivingEntity& passenger) const override;
        bool IsPushable() const override { return true; }
        double GetGravity() const override { return IsInWater() ? 0.005 : 0.04; }   // getDefaultGravity
        float GetAirDrag() const override { return 0.95f; }
        float GetBlockSpeedFactor() const override;
        bool EmitsMovementSounds() const override { return false; }   // MovementEmission.EVENTS
        // MC AbstractMinecart.push(Entity) — server side: carts push carts
        // along the track, everything else is shoved (a quarter each way).
        void PushEntity(Entity& other) override;
        glm::dvec3 GetKnownMovement() const override;

        // ── The rail ───────────────────────────────────────────────────────
        bool IsOnRails() const { return m_onRails; }
        bool IsFlipped() const { return m_flipped; }
        void SetFlipped(bool f) { m_flipped = f; }
        // MC getCurrentBlockPosOrRailBelow (old movement).
        glm::ivec3 GetCurrentBlockPosOrRailBelow() const;
        // MC OldMinecartBehavior.getPos / getPosOffs: the point on the rail
        // under (x, y, z) — the renderer's rail snap too. False off-rail.
        static bool GetRailPos(const IBlockAccess& level, double x, double y, double z, glm::dvec3& out);
        static bool GetRailPosOffs(const IBlockAccess& level, double x, double y, double z, double offs,
                                   glm::dvec3& out);
        // MC getMotionDirection (old): the cart's heading as a horizontal
        // direction ordinal (Game::Direction), flipped with it.
        int GetMotionDirectionIndex() const;

        // MC activateMinecart(level, x, y, z, powered) — an activator rail.
        virtual void ActivateMinecart(int x, int y, int z, bool powered) { (void)x; (void)y; (void)z; (void)powered; }
        // MC isRideable / isFurnace.
        virtual bool IsRideable() const { return false; }
        virtual bool IsFurnace() const { return false; }

        // ── The displayed block (MC DATA_ID_CUSTOM_DISPLAY_BLOCK / OFFSET) ─
        BlockState GetDisplayBlockState() const {
            return m_hasCustomDisplay ? m_customDisplay : GetDefaultDisplayBlockState();
        }
        virtual BlockState GetDefaultDisplayBlockState() const;
        bool HasCustomDisplayBlockState() const { return m_hasCustomDisplay; }
        BlockState CustomDisplayBlockState() const { return m_customDisplay; }
        void SetCustomDisplayBlockState(std::optional<BlockState> state) {
            m_hasCustomDisplay = state.has_value();
            m_customDisplay = state.value_or(BlockState{});
        }
        int  GetDisplayOffset() const { return m_displayOffset; }
        void SetDisplayOffset(int offset) { m_displayOffset = offset; }
        virtual int GetDefaultDisplayOffset() const { return 6; }


        // ── Wire / save ────────────────────────────────────────────────────
        void FillSyncedData(VehicleSyncedData& out) const override;
        void ApplySyncedData(const VehicleSyncedData& in) override;
        // "HasTicked" is MC's firstTick (inverted), saved.
        bool FirstTickFlag() const { return firstTick; }
        void SetFirstTickFlag(bool v) { firstTick = v; }

        // A client's copy never simulates (no controlling passenger); it
        // eases to the server's positions and keeps its own heading
        // (OldMinecartBehavior.tick's client branch).
        void ClientSnapshotMotion(const glm::dvec3& v) { m_targetDeltaMovement = v; }

    protected:
        AbstractMinecart(EntityTypeId type, EntityLevel* level);

        // MC getMaxSpeed(level).
        virtual double GetMaxSpeed() const { return IsInWater() ? kMaxSpeedInWater : kMaxSpeedOnLand; }
        // MC applyNaturalSlowdown(movement).
        virtual glm::dvec3 ApplyNaturalSlowdown(const glm::dvec3& movement) const;
        // OldMinecartBehavior.getSlowdownFactor.
        double GetSlowdownFactor() const { return IsVehicle() || HasPlayerPassenger() ? 0.997 : 0.96; }
        // A tick-local hook the hopper cart uses (makeStepAlongTrack's
        // tryConsumeItems has no old-movement counterpart).
        virtual void AfterBehaviorTick() {}

        bool IsRedstoneConductor(const glm::ivec3& pos) const;

    private:
        // OldMinecartBehavior.
        void BehaviorTick();
        void MoveAlongTrack();
        void ComeOffTrack();
        bool PushAndPickupEntities();
        void PushOtherMinecart(AbstractMinecart& other, double xa, double za);

        bool m_onRails = false;
        bool m_flipped = false;          // FlippedRotation
        bool       m_hasCustomDisplay = false;
        BlockState m_customDisplay{};
        int        m_displayOffset = 6;
        glm::dvec3 m_targetDeltaMovement{0.0};   // OldMinecartBehavior.targetDeltaMovement
    };

    // ── Minecart ────────────────────────────────────────────────────────────
    class Minecart final : public AbstractMinecart {
    public:
        explicit Minecart(EntityLevel* level);
        ItemID GetDropItem() const override;
        bool IsRideable() const override { return true; }
        int  GetMaxPassengers() const override { return 1; }
        void ActivateMinecart(int x, int y, int z, bool powered) override;
        UseResult VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) override;
    };

    // ── AbstractMinecartContainer ───────────────────────────────────────────
    class MinecartContainerBase : public AbstractMinecart {
    public:
        VehicleContainer&       Container()       { return m_container; }
        const VehicleContainer& Container() const { return m_container; }
        VehicleContainer* GetVehicleContainer() override { return &m_container; }
        MenuType GetContainerMenuType() const override {
            return GetType() == EntityTypeId::HopperMinecart ? MenuType::Hopper : MenuType::Generic9x3;
        }
        // MinecartChest.stopOpen: CONTAINER_CLOSE (the hopper cart has none).
        void StopOpenContainer(LivingEntity& player) override;
        // AbstractMinecartContainer.remove: a destroying removal pours the
        // contents out.
        void OnRemoving(RemovalReason reason) override;
        // AbstractMinecartContainer.destroy: the cart, then the contents.
        void Destroy(MobDamageSource source, Entity* attacker) override;
        UseResult VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) override;
    protected:
        MinecartContainerBase(EntityTypeId type, EntityLevel* level, int size);
        glm::dvec3 ApplyNaturalSlowdown(const glm::dvec3& movement) const override;
        VehicleContainer m_container;
    };

    class MinecartChest final : public MinecartContainerBase {
    public:
        explicit MinecartChest(EntityLevel* level);
        ItemID GetDropItem() const override;
        BlockState GetDefaultDisplayBlockState() const override;
        int GetDefaultDisplayOffset() const override { return 8; }
    };

    class MinecartHopper final : public MinecartContainerBase {
    public:
        explicit MinecartHopper(EntityLevel* level);
        ItemID GetDropItem() const override;
        BlockState GetDefaultDisplayBlockState() const override;
        int GetDefaultDisplayOffset() const override { return 1; }
        void ActivateMinecart(int x, int y, int z, bool powered) override;
        bool IsEnabled() const { return m_enabled; }
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        void Tick() override;
        // MC Hopper.getLevelX/Y/Z — the funnel's point.
        glm::dvec3 HopperLevelPos() const { return glm::dvec3(position.x, position.y + 0.5, position.z); }
        // MC MinecartHopper.suckInItems.
        bool SuckInItems();
    protected:
        void AfterBehaviorTick() override { TryConsumeItems(); }
    private:
        void TryConsumeItems();
        bool m_enabled = true;
        bool m_consumedItemThisFrame = false;
    };

    class MinecartFurnace final : public AbstractMinecart {
    public:
        explicit MinecartFurnace(EntityLevel* level);
        static constexpr int kFuelTicksPerItem = 3600;   // FUEL_TICKS_PER_ITEM
        static constexpr int kMaxFuelTicks     = 32000;  // MAX_FUEL_TICKS
        ItemID GetDropItem() const override;
        bool IsFurnace() const override { return true; }
        BlockState GetDefaultDisplayBlockState() const override;
        void Tick() override;
        UseResult VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) override;
        // MC addFuel(interactingPos, itemStack).
        bool AddFuel(const glm::dvec3& interactingPos, const ItemStack& itemStack);
        bool HasFuel() const { return m_hasFuel; }
        int  GetFuel() const { return m_fuel; }
        void SetFuel(int fuel) { m_fuel = fuel; }
        const glm::dvec3& GetPush() const { return m_push; }
        void SetPush(const glm::dvec3& push) { m_push = push; }
        void FillSyncedData(VehicleSyncedData& out) const override;
        void ApplySyncedData(const VehicleSyncedData& in) override;
    protected:
        double GetMaxSpeed() const override;
        glm::dvec3 ApplyNaturalSlowdown(const glm::dvec3& movement) const override;
    private:
        int        m_fuel = 0;
        glm::dvec3 m_push{0.0};
        bool       m_hasFuel = false;   // DATA_ID_FUEL
    };

    class MinecartTNT final : public AbstractMinecart {
    public:
        explicit MinecartTNT(EntityLevel* level);
        static constexpr float kDefaultExplosionPowerBase = 4.0f;
        static constexpr float kDefaultExplosionSpeedFactor = 1.0f;
        ItemID GetDropItem() const override;
        BlockState GetDefaultDisplayBlockState() const override;
        void Tick() override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void Destroy(MobDamageSource source, Entity* attacker) override;
        bool ShouldSourceDestroy(MobDamageSource source, const Entity* direct) const override;
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;
        void ActivateMinecart(int x, int y, int z, bool powered) override;
        void HandleEntityEvent(uint8_t id) override;
        // The igniter (kill credit) is a raw pointer: forgotten when it goes.
        void ClearReferenceTo(const Entity* entity) override;
        // MC primeFuse(source).
        void PrimeFuse(Entity* igniter);
        int  GetFuse() const { return m_fuse; }
        void SetFuse(int f) { m_fuse = f; }
        bool IsPrimed() const { return m_fuse > -1; }
        float ExplosionPowerBase() const { return m_explosionPowerBase; }
        float ExplosionSpeedFactor() const { return m_explosionSpeedFactor; }
        void SetExplosionPowerBase(float v);
        void SetExplosionSpeedFactor(float v);
        // The fuse rides the animation byte on the wire (the client counts it
        // down itself after entity event 70).
        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;
        bool    AnimStateTicksOnClient() const override { return true; }
    private:
        void Explode(double speedSqr);
        // MC damageSourceIgnitesTnt(source).
        static bool DamageSourceIgnitesTnt(MobDamageSource source, const Entity* direct);
        int   m_fuse = -1;
        float m_explosionPowerBase = kDefaultExplosionPowerBase;
        float m_explosionSpeedFactor = kDefaultExplosionSpeedFactor;
        Entity* m_igniter = nullptr;   // ignitionSource's causing entity (kill credit)
    };

    class MinecartSpawner final : public AbstractMinecart {
    public:
        explicit MinecartSpawner(EntityLevel* level);
        ~MinecartSpawner() override;
        ItemID GetDropItem() const override;
        ItemStack GetPickResult() const override;
        BlockState GetDefaultDisplayBlockState() const override;
        void Tick() override;
        // The BaseSpawner (MC's anonymous subclass) — the spawner block
        // entity's logic, run at the cart's cell.
        SpawnerBlockEntity& Spawner() { return *m_spawner; }
        const SpawnerBlockEntity& Spawner() const { return *m_spawner; }
    private:
        std::unique_ptr<SpawnerBlockEntity> m_spawner;
    };

    class MinecartCommandBlock final : public AbstractMinecart {
    public:
        explicit MinecartCommandBlock(EntityLevel* level);
        static constexpr int kActivationDelay = 4;   // ACTIVATION_DELAY
        ItemID GetDropItem() const override;
        ItemStack GetPickResult() const override;
        BlockState GetDefaultDisplayBlockState() const override;
        void ActivateMinecart(int x, int y, int z, bool powered) override;

        // MC BaseCommandBlock.
        const std::string& GetCommand() const { return m_command; }
        void SetCommand(std::string command) { m_command = std::move(command); m_successCount = 0; }
        int  GetSuccessCount() const { return m_successCount; }
        void SetSuccessCount(int n) { m_successCount = n; }
        bool TrackOutput() const { return m_trackOutput; }
        void SetTrackOutput(bool v) { m_trackOutput = v; }
        const std::string& GetLastOutput() const { return m_lastOutput; }
        void SetLastOutput(std::string out) { m_lastOutput = std::move(out); }
        int64_t GetLastExecution() const { return m_lastExecution; }
        void SetLastExecution(int64_t t) { m_lastExecution = t; }
        bool UpdateLastExecution() const { return m_updateLastExecution; }
        void SetUpdateLastExecution(bool v) { m_updateLastExecution = v; }

        // The server runs a command for a command block minecart:
        // performPrefixedCommand as the cart (gamemaster permissions, its
        // position), returning how many commands succeeded and the output.
        using CommandRunner = int (*)(MinecartCommandBlock& cart, const std::string& command,
                                      std::string& output);
        static void SetCommandRunner(CommandRunner runner);

    private:
        // MC BaseCommandBlock.performCommand.
        bool PerformCommand();
        std::string m_command;
        std::string m_lastOutput;
        int     m_successCount = 0;
        bool    m_trackOutput = true;
        bool    m_updateLastExecution = true;
        int64_t m_lastExecution = -1;
        int     m_lastActivated = 0;
    };

    // Build the minecart entity for a type (MinecartItem / /summon / load).
    std::unique_ptr<AbstractMinecart> CreateMinecart(EntityTypeId type, EntityLevel* level);

} // namespace Game
