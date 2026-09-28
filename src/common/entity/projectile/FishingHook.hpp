// File: src/common/entity/projectile/FishingHook.hpp
//
// MC net.minecraft.world.entity.projectile.FishingHook — the bobber a fishing
// rod casts (EntityTypes.FISHING_BOBBER: MISC, 0.25 x 0.25, tracking range 4,
// update interval 5, noSave, noSummon).
//
// The state machine is FishingHook.tick transcribed:
//   FLYING            gravity 0.03, the hit clip (entities, dropped items,
//                     blocks); water turns it into BOBBING, an entity hit into
//                     HOOKED_IN_ENTITY; grounded 1200 ticks and it is gone.
//   HOOKED_IN_ENTITY  rides the hooked entity at 0.8 of its height; if that
//                     entity goes (removed, can no longer interact, another
//                     dimension) the hook drops back to FLYING.
//   BOBBING           the buoyancy spring against the water surface, the
//                     open-water survey (a 5x4x5 block of water below open
//                     air, which the treasure pool requires), and on the
//                     server the fish: time until lured (100..600 - Lure*100
//                     ticks, rain speeds it, a roof slows it), the approaching
//                     wake (FISHING particles walking in toward the bobber),
//                     the bite (splash sound, BUBBLE + FISHING burst, the
//                     bobber dip) and the 20..40 tick catch window.
//
// Reeling in (FishingRodItem.use's retrieve) needs the server's item, orb and
// player machinery, so it lives in server/level/FishingRodUse.cpp — the same
// common-declares / server-defines split the ender pearl uses. It reads the
// hook through the accessors below.
//
// SYNCED DATA. MC's DATA_HOOKED_ENTITY and DATA_BITING, plus the owner id
// MC sends in ClientboundAddEntityPacket's data int and the holding arm the
// renderer derives from the owner's hands (which a remote client does not
// know here), ride FishingHookDataS2C (packets/game/FishingHookDataS2CPacket
// .hpp): on tracking start and whenever any of them changes.
//
// ENGINE DIVERGENCES, each at its site:
//   * players, mobs and dropped items are different kinds of object here, so
//     the hooked target is either an Entity (a mob or a player's view) or a
//     dropped item's id;
//   * MC's syncronizedRandom is seeded from the UUID, which the client never
//     learns here — both sides seed from the entity id instead (it only
//     shapes the bite dip, identically on both sides);
//   * no weather: `isRainingAt` asks the level, which answers false.
#pragma once

#include "common/core/JavaRandom.hpp"
#include "common/entity/projectile/Projectile.hpp"

#include <cstdint>

namespace Game {

    struct ItemStack;
    class ILevelWrite;
    class IUsePlayer;
    enum class UseResult : int;

    class FishingHook : public Projectile {
    public:
        // MC FishingHook.FishHookState.
        enum class State : uint8_t { Flying, HookedInEntity, Bobbing };

        // FishingHook.MAX_OUT_OF_WATER_TIME / the grounded despawn.
        static constexpr int kMaxOutOfWaterTime = 10;
        static constexpr int kGroundedLife      = 1200;
        // shouldStopFishing: 32 blocks from the owner.
        static constexpr double kMaxOwnerDistanceSq = 1024.0;
        // Entity event 31: reel the hooked entity toward the owner (both
        // sides; the client half is what moves a hooked PLAYER).
        static constexpr uint8_t kEventPullHooked = 31;

        explicit FishingHook(EntityLevel* level);
        ~FishingHook() override;

        // MC FishingHook(Player, Level, luck, lureSpeed): owner, the spawn
        // point 0.3 behind the eye, and the cast velocity (0.6 plus a little
        // triangle noise per axis, pitch clamped to +-5 : 1).
        void InitFromPlayer(LivingEntity& player, float playerYRot, float playerXRot,
                            int luck, int lureSpeed);

        void Tick() override;
        void HandleEntityEvent(uint8_t id) override;

        // noSave; not pickable, not pushable, no step height, no movement
        // sounds (MovementEmission.NONE); canUsePortal false.
        bool  CanSerialize() const override { return false; }
        bool  IsPickable() const override { return false; }
        bool  IsPushable() const override { return false; }
        float MaxUpStep() const override { return 0.0f; }
        bool  EmitsMovementSounds() const override { return false; }
        bool  CanUsePortal(bool ignorePassenger) const override {
            (void)ignorePassenger;
            return false;
        }

        void ClearReferenceTo(const Entity* entity) override;

        // ── What the reel-in reads (server) ────────────────────────────────
        State GetState() const { return m_state; }
        bool  IsOpenWaterFishing() const { return m_openWater; }
        int   GetLuck() const { return m_luck; }
        int   GetLureSpeed() const { return m_lureSpeed; }
        // A fish is on the line: the bite's catch window.
        bool  IsNibbling() const { return m_nibble > 0; }
        // The hooked target: an Entity (mob or player view), or a dropped
        // item's id (0 = none). At most one of the two is set.
        Entity* GetHookedIn() const { return m_hookedIn; }
        int32_t GetHookedItemId() const { return m_hookedItemId; }
        bool    HasHookedTarget() const { return m_hookedIn != nullptr || m_hookedItemId != 0; }

        // MC getPlayerOwner — the owner when it is a player, else null.
        // Server side only (the client knows its owner by id).
        LivingEntity* GetPlayerOwner();

        // MC shouldStopFishing: the owner can no longer interact with this
        // level, holds no rod in either hand, or is more than 32 blocks away
        // — the hook is discarded and true returned. Server side.
        bool ShouldStopFishing(LivingEntity& owner);

        // MC pullEntity for the hooked target: 0.1 of the way to the owner
        // added to its motion. Server: mobs and dropped items (a player's
        // motion is theirs — their own client pulls on event 31).
        void PullHooked();

        // ── Synced data (FishingHookDataS2C) ───────────────────────────────
        int32_t GetOwnerNetId() const { return m_ownerNetId; }
        int32_t GetHookedNetId() const { return m_hookedNetId; }
        bool    IsBiting() const { return m_biting; }
        // 0 = the rod is in the main hand (MC getHoldingArm → main arm), 1 =
        // the off hand.
        uint8_t GetOwnerHand() const { return m_ownerHand; }
        // The tracker's send-on-change latch.
        bool ConsumeSyncDirty() {
            const bool d = m_syncDirty;
            m_syncDirty = false;
            return d;
        }
        // Client: FishingHookDataS2C arrived (MC onSyncedDataUpdated — the
        // bite dips the bobber).
        void ApplySyncedData(int32_t ownerNetId, int32_t hookedNetId, bool biting, uint8_t ownerHand);

        // ── MC Player.fishing ──────────────────────────────────────────────
        // The live hook a player owns, by the player's net id (connection
        // id), on one side. `level` narrows it to one level (null: any).
        // Never a removed hook. Each side only ever asks for its own hooks.
        static FishingHook* FindForOwner(bool clientSide, int32_t ownerNetId,
                                         const EntityLevel* level = nullptr);

    protected:
        // MC canHitEntity: every projectile target that is pickable (MC's
        // canBeHitByProjectile), plus dropped items (CheckCollision).
        bool CanHitEntity(const Entity& entity) const override;
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHitBlock(const HitResult& hit) override;

    private:
        void SetHookedEntity(Entity* entity);
        void SetHookedItem(int32_t itemId);
        void ClearHooked();
        void SetBiting(bool biting);
        void CheckCollision();
        void CatchingFish(const glm::ivec3& blockPos);
        bool CalculateOpenWater(const glm::ivec3& blockPos) const;
        void Register(int32_t ownerNetId);
        void Unregister();
        // Where the hooked target is, and whether it is still hookable.
        bool ResolveHookedTarget(glm::dvec3& feet, double& height) const;

        JavaRandom m_syncRandom{0};
        bool    m_biting = false;
        int     m_outOfWaterTime = 0;
        int     m_life = 0;
        int     m_nibble = 0;
        int     m_timeUntilLured = 0;
        int     m_timeUntilHooked = 0;
        float   m_fishAngle = 0.0f;
        bool    m_openWater = true;
        bool    m_hasBeenShot = false;
        State   m_state = State::Flying;
        int     m_luck = 0;
        int     m_lureSpeed = 0;

        Entity* m_hookedIn = nullptr;
        int32_t m_hookedItemId = 0;
        // The owner as cast — the player's view in THIS level. Its retirement
        // (the player left the level, logged out, or came back after leaving)
        // is MC's getOwner() turning null here: the line is gone.
        const Entity* m_castBy = nullptr;

        int32_t m_ownerNetId = -1;
        int32_t m_hookedNetId = -1;
        uint8_t m_ownerHand = 0;
        bool    m_syncDirty = true;
        bool    m_registered = false;
    };

    // FishingRodItem.use's server half (server/level/FishingRodUse.cpp):
    // reel in the player's hook when there is one (retrieve, the rod worn by
    // what it returns, FISHING_BOBBER_RETRIEVE), else cast a new one
    // (FISHING_BOBBER_THROW, Lure and Luck of the Sea read off the rod).
    // Always SUCCESS, as in MC.
    UseResult UseFishingRodServer(ILevelWrite* world, IUsePlayer& player, uint32_t hand,
                                  ItemStack& rod);

} // namespace Game
