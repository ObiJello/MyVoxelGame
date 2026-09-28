// File: src/common/entity/vehicle/Boat.hpp
//
// MC net.minecraft.world.entity.vehicle.boat — AbstractBoat and its four
// shapes, in one class keyed on the entity type:
//
//   Boat       the eleven wooden boats (rideHeight = height / 3)
//   Raft       the bamboo raft          (rideHeight = height * 0.8888889)
//   ChestBoat  AbstractChestBoat: one seat 0.15 aft, a 27-slot chest
//   ChestRaft  the bamboo chest raft
//
// PHYSICS (AbstractBoat.tick / getStatus / floatBoat / controlBoat), exact:
// the five statuses (IN_WATER, UNDER_WATER, UNDER_FLOWING_WATER, ON_LAND,
// IN_AIR); gravity 0.04; the float — buoyancy (waterLevel − y) / height
// scaled by 0.04/0.65 and damped by 0.75, friction 0.9 on water, 0.45 under
// it, the ground friction averaged over the blocks under the hull (ice 0.98,
// blue ice 0.989 — the ice-boat roads) halved into the next tick while a
// player drives; the paddles: turning ±1°/tick of deltaRotation, 0.04
// forward, 0.005 back, 0.005 turning in place; out of control after 60
// ticks under water (the riders are thrown off); bubble columns (a push up
// or a drag down after 60 ticks above one).
//
// WHO SIMULATES: the driving player's client (VehicleEntity header); the
// server simulates an undriven boat (drifting, carrying mobs) and applies the
// driver's reported moves otherwise.
#pragma once

#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/entity/vehicle/VehicleContainer.hpp"

#include <array>
#include <memory>

namespace Game {

    class Boat final : public VehicleEntity {
    public:
        Boat(EntityTypeId type, EntityLevel* level);
        ~Boat() override;

        // MC AbstractBoat.Status, ordinals verbatim.
        enum class Status : uint8_t { InWater = 0, UnderWater, UnderFlowingWater, OnLand, InAir };

        // ── MC constants ───────────────────────────────────────────────────
        static constexpr int    kTimeToEject     = 60;              // TIME_TO_EJECT
        static constexpr float  kPaddleSpeed     = 0.3926991f;      // PADDLE_SPEED (π/8)
        static constexpr double kPaddleSoundTime = 0.7853981852531433; // PADDLE_SOUND_TIME (π/4)
        static constexpr int    kBubbleTime      = 60;              // BUBBLE_TIME
        static constexpr int    kChestSize       = 27;              // AbstractChestBoat.CONTAINER_SIZE

        bool IsChest() const { return m_container != nullptr; }
        bool IsRaft() const { return IsRaftEntityType(GetType()); }
        VehicleContainer*       Container()       { return m_container.get(); }
        const VehicleContainer* Container() const { return m_container.get(); }
        VehicleContainer* GetVehicleContainer() override { return m_container.get(); }
        // AbstractChestBoat.stopOpen: CONTAINER_CLOSE.
        void StopOpenContainer(LivingEntity& player) override;
        // MC AbstractBoat.remove / AbstractChestBoat.remove: a destroying
        // removal (killed, discarded) drops the lead and pours the chest out.
        void OnRemoving(RemovalReason reason) override;
        // MC AbstractBoat.interact / AbstractChestBoat.interact.
        UseResult VehicleInteract(LivingEntity& player, ItemStack& held, bool sneaking) override;

        // MC setInitialPos: the position and xo/yo/zo.
        void SetInitialPos(const glm::dvec3& pos);

        // ── Entity hooks ───────────────────────────────────────────────────
        void Tick() override;
        void BaseTick() override;
        void HandleEntityEvent(uint8_t id) override;
        void PositionRider(Entity& passenger) override;
        glm::dvec3 GetDismountLocationForPassenger(const LivingEntity& passenger) const override;
        glm::dvec3 PassengerAttachmentAt(int slot, int total, bool animalPassenger,
                                         float vehicleYRot) const override;
        bool CanAddPassenger(const Entity& passenger) const override;
        int  GetMaxPassengers() const override { return IsChest() ? 1 : 2; }
        bool IsControlledByPlayer() const override { return FirstPassengerIsPlayer(); }
        bool IsPushable() const override { return true; }
        // MC AbstractBoat.push(Entity): another boat pushes when it overlaps
        // this one's height; anything else only from at or below its floor.
        void PushEntity(Entity& other) override;
        double GetGravity() const override { return 0.04; }   // getDefaultGravity
        bool EmitsMovementSounds() const override { return false; }   // MovementEmission.EVENTS
        // MC AbstractBoat.canBeCollidedWith: solid to whatever meets it.
        bool CanBeCollidedWith() const override { return true; }

        // MC AbstractBoat.hasEnoughSpaceFor: narrower than the boat.
        bool HasEnoughSpaceFor(const Entity& entity) const {
            return entity.GetBbWidth() < GetBbWidth();
        }

        // ── Breaking ───────────────────────────────────────────────────────
        ItemID GetDropItem() const override { return BoatItemForType(GetType()); }
        // AbstractChestBoat.destroy: the boat item, then the chest's contents.
        void Destroy(MobDamageSource source, Entity* attacker) override;

        // ── Driving (the controlling client) ───────────────────────────────
        // MC setInput(left, right, up, down) — LocalPlayer.rideTick.
        void SetInput(bool left, bool right, bool up, bool down) {
            m_inputLeft = left; m_inputRight = right; m_inputUp = up; m_inputDown = down;
        }
        // MC setPaddleState / getPaddleState(side): the synched flags, and a
        // paddle only rows while someone controls the boat.
        void SetPaddleState(bool left, bool right, bool reverse = false) {
            m_paddleLeft = left;
            m_paddleRight = right;
            m_paddleReverse = reverse && (left || right);
        }
        // Back-paddling (the engine's deliberate addition to MC): driven
        // backward, both paddles row the other way round.
        bool IsPaddlingInReverse() const { return m_paddleReverse; }
        bool GetPaddleState(int side) const;
        bool RawPaddleState(int side) const { return side == 0 ? m_paddleLeft : m_paddleRight; }
        // MC getRowingTime(side, a): the paddle's phase between ticks.
        float GetRowingTime(int side, float a) const;
        // MC deltaRotation — the turn the boat made this tick, which its
        // driver's view is carried round by.
        float DeltaRotation() const { return m_deltaRotation; }
        // MC getBubbleAngle(a) and isUnderWater (the renderer's tilt and the
        // water patch).
        float GetBubbleAngle(float a) const;
        bool  IsBoatUnderWater() const {
            return m_status == Status::UnderWater || m_status == Status::UnderFlowingWater;
        }
        Status GetStatus() const { return m_status; }
        float  OutOfControlTicks() const { return m_outOfControlTicks; }

        // MC AbstractBoat.onAboveBubbleColumn / Entity.onInsideBubbleColumn.
        void OnAboveBubbleColumn(bool dragDown, const glm::ivec3& pos);

        // MC clampRotation(passenger) — keep a non-turning rider facing
        // within 105° of the boat; returns the correction applied. The
        // local client runs the same clamp on its player's view.
        static float ClampPassengerYaw(float passengerYRot, float boatBodyYRot);

        // ── Wire ───────────────────────────────────────────────────────────
        void FillSyncedData(VehicleSyncedData& out) const override;
        void ApplySyncedData(const VehicleSyncedData& in) override;

        // ── Save (EntityNbt) ───────────────────────────────────────────────
        // MC readAdditionalSaveData is the leash (Mob's layer) plus, for a
        // chest boat, ContainerEntity's items / loot table.

        // MC Leashable: boats take a lead (and a happy ghast's four).
        bool IsLeashable() const override { return true; }
        bool CanBeLeashed() const override { return true; }
        bool SupportQuadLeash() const override { return true; }
        std::array<glm::dvec3, 4> GetQuadLeashOffsets() const override;
        glm::dvec3 GetLeashOffset(float partialTicks) const override;
        void CloseRangeLeashBehaviour(Entity& holder) override { (void)holder; }
        void WhenLeashedTo(Entity& holder) override { (void)holder; }
        void OnLeashRemoved() override {}
        void LeashTooFarBehaviour() override;

    private:
        // MC getStatus / isUnderwater / checkInWater / getGroundFriction /
        // getWaterLevelAbove.
        Status ComputeStatus();
        std::optional<Status> IsUnderwaterStatus();
        bool CheckInWater();
        float GetGroundFriction() const;
        float GetWaterLevelAbove() const;
        void FloatBoat();
        void ControlBoat();
        void TickBubbleColumn();
        void HandleBubbleColumnEffect(bool dragDown);
        // Bubble columns under the hull this tick (MC applyEffectsFromBlocks
        // → BubbleColumnBlock.entityInside).
        void ApplyBubbleColumns();
        void PushAndPullEntities();
        // MC AbstractBoat.checkFallDamage — a landing boat hurts nothing.
        void VehicleCheckFallDamage(double ya, bool onGroundNow) override;
        // MC rideHeight(dimensions).
        double RideHeight() const;
        // MC calculatePassengerBodyYRot — an animal in a full boat sits
        // sideways.
        float PassengerBodyYRot(bool animal, int32_t passengerId) const;

        std::unique_ptr<VehicleContainer> m_container;

        std::array<float, 2> m_paddlePositions{0.0f, 0.0f};
        float  m_outOfControlTicks = 0.0f;
        float  m_deltaRotation = 0.0f;
        bool   m_inputLeft = false, m_inputRight = false, m_inputUp = false, m_inputDown = false;
        double m_waterLevel = 0.0;
        float  m_landFriction = 0.0f;
        Status m_status = Status::InAir;
        Status m_oldStatus = Status::InAir;
        double m_lastYd = 0.0;
        bool   m_isAboveBubbleColumn = false;
        bool   m_bubbleColumnDirectionIsDown = false;
        float  m_bubbleMultiplier = 0.0f;
        float  m_bubbleAngle = 0.0f;
        float  m_bubbleAngleO = 0.0f;

        // Synched (VehicleSyncedData).
        bool m_paddleLeft = false;
        bool m_paddleRight = false;
        bool m_paddleReverse = false;
        int  m_bubbleTime = 0;
    };

} // namespace Game
