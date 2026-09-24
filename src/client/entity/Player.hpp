// File: src/client/entity/Player.hpp
#pragma once

#include "common/world/level/DimensionId.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/PlayerColors.hpp"
#include "common/physics/RayCast.hpp"
#include "common/physics/Physics.hpp"
#include "common/entity/LivingEntity.hpp"   // WalkAnimationState (morph)
#include "common/entity/Morph.hpp"
#include "../renderer/core/Camera.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <chrono>
#include <vector>

namespace Game {

    // Forward declarations
    class World;
    struct IBlockAccess;


    // Player statistics tracking
    struct PlayerStats {
        int blocksPlaced = 0;
        int blocksBroken = 0;
        int lastPlacedBlockId = -1;
        int lastBrokenBlockId = -1;
        float totalDistanceTraveled = 0.0f;
        float totalPlayTime = 0.0f;
    };

    // Client-side player entity that holds authoritative visual state
    class ClientPlayer {
    public:
        ClientPlayer();

        // === Core State ===
        
        // Physics state (authoritative for client)
        PlayerPhysics physics;
        
        // Transform tracking for client-server sync
        glm::dvec3 serverPos{0.0, 67.0, 0.0};     // Last server-confirmed position
        glm::dvec3 predictedPos{0.0, 67.0, 0.0};  // Local predicted position
        float yaw = 0.0f;                         // Camera yaw (degrees)
        float pitch = 0.0f;                       // Camera pitch (degrees)
        
        // /morph: what this player has become (Game::Morph code, kNone =
        // none). Set from the abilities packet; the physics body follows
        // (PlayerPhysics::SetMorph) and the third-person view draws the
        // body with `morphWalk` for a mob's limbs.
        uint32_t                 morph = Game::Morph::kNone;
        Game::WalkAnimationState morphWalk;
        int                      morphTicks = 0;
        glm::dvec3               morphPrevPos{0.0};
        bool IsMorphed() const { return !Game::Morph::IsNone(morph); }
        bool IsBlockMorph() const { return IsMorphed() && Game::Morph::KindOf(morph) == Game::Morph::Kind::Block; }
        bool IsCreeperMorph() const { return Game::Morph::IsMob(morph, Game::EntityTypeId::Creeper); }
        // Creeper morph: Shift+Space held swells it (MC Creeper.swell, +1 a
        // tick to 30), released it calms (-1 a tick). At 30 TickMorph
        // reports the bang once — the host loop sends `/morph boom` — and
        // the swell resets. `morphSwellOld` is the previous tick's, for
        // the renderer's partial-tick lerp (Creeper.getSwelling).
        int  morphSwell = 0;
        int  morphSwellOld = 0;
        // Other mobs' one-shot animations: a sheep's graze (kSheepEatTicks
        // down to 0), a skeleton's drawn bow (kSkeletonAimTicks). Started
        // by Left Alt; counted down in TickMorph; `Old` for the lerp.
        int  morphAnimTicks = 0;
        int  morphAnimTicksOld = 0;
        void StartMorphAnim(int ticks) { morphAnimTicks = morphAnimTicksOld = ticks; }
        // The byte the move packet carries for the others: the creeper's
        // swell, or the one-shot animation's remaining ticks.
        uint8_t MorphAnimByte() const {
            return static_cast<uint8_t>(IsCreeperMorph() ? morphSwell : morphAnimTicks);
        }
        void SetMorph(uint32_t code, float walkSpeed);
        // Block morph, Left Alt: the body snaps to the block grid (the cell's
        // centre, an integer floor) and holds there — no input, no gravity —
        // so the block reads as placed. Alt again releases it.
        bool       morphGridLock = false;
        // /morph item: carried by this player (0 = not). The body rides on
        // the holder; MorphHeldS2C sets and clears it.
        uint32_t   heldByPlayer = 0;
        glm::dvec3 morphLockPos{0.0};
        // The server put the real block in the locked cell and this client
        // has seen it there; when the cell empties after that, someone
        // mined it and the lock is over.
        bool       morphLockBlockSeen = false;
        void SetMorphGridLock(bool on);
        // Once per client tick: MC LivingEntity.updateWalkAnimation for the
        // morph's limbs, from the tick's horizontal travel; the body yaw
        // (LivingEntity.tickHeadTurn: follows the travel direction, stays
        // put while still, dragged only past ±50° of `headYaw`) for the
        // third-person view; and the creeper swell. Returns true on the
        // tick the creeper reaches full swell.
        bool TickMorph(float headYaw);
        float morphBodyYaw = 0.0f;
        float morphBodyYawOld = 0.0f;

        // Visual smoothing (for interpolation)
        glm::dvec3 visualPos{0.0, 67.0, 0.0};     // Smoothed position for rendering
        float visualYaw = 0.0f;                   // Smoothed yaw
        float visualPitch = 0.0f;                 // Smoothed pitch

        // Stick-figure render color — set from launcher's --color CLI arg at startup,
        // sent to the server at handshake so other clients render this player in the
        // chosen color too. Default = the historical neon green.
        Game::PlayerColorId color = Game::PlayerColorId::Default;
        
        // === Player Attributes ===

        // Status — synced from the server via SetHealthS2C
        // (ClientPacketHandler::handleSetHealth); read by the HUD.
        int   health     = 20;
        int   food       = 20;
        float saturation = 5.0f;
        // The HUD's extras, appended to SetHealthS2C: absorption hearts, the
        // effect / hardcore flags (SetHealthS2CPacket::kFlag*) and the air
        // bar (MC DATA_AIR_SUPPLY_ID).
        float   absorption = 0.0f;
        uint8_t hudFlags   = 0;
        int     airSupply  = 300;
        // MC LivingEntity.invulnerableTime as the HUD sees it (Gui reads
        // player.damageCooldownTime): 20 ticks from the hurt animation
        // packet, counted down in Tick. The heart blink keys on it.
        int     damageCooldownTime = 0;
        // XP — synced via SetExperienceS2C (handleSetExperience); the HUD's
        // bar fill and level number read these each frame.
        float xpProgress = 0.0f;
        int   xpLevel    = 0;
        int   air = 300;           // TODO: Sync from server (ticks of air remaining)

        // ── Status effects (MC LocalPlayer's activeEffects) ──────────────
        // Filled by UpdateMobEffectS2C (MC handleUpdateMobEffect →
        // forceAddEffect) and RemoveMobEffectS2C (removeEffectNoUpdate);
        // counted down and blended client-side in TickEffects (MC
        // MobEffectInstance.tickClient). Read by the HUD icons, the inventory
        // effect list, the fog / night vision / nausea hooks and — through
        // ApplyEffectPhysics — the local physics.
        std::vector<Game::MobEffectInstance> activeEffects;
        const Game::MobEffectInstance* GetEffect(Game::MobEffectId id) const {
            return Game::FindEffectIn(activeEffects, id);
        }
        bool HasEffect(Game::MobEffectId id) const { return GetEffect(id) != nullptr; }
        // MC LivingEntity.getEffectBlendFactor.
        float GetEffectBlendFactor(Game::MobEffectId id, float partialTick) const {
            const Game::MobEffectInstance* e = GetEffect(id);
            return e ? e->GetBlendFactor(partialTick) : 0.0f;
        }
        // MC handleUpdateMobEffect: forceAddEffect with the previous
        // instance's blend state (skipBlending when the packet says no blend).
        void ApplyEffectUpdate(Game::MobEffectInstance effect, bool blend);
        // MC handleRemoveMobEffect → removeEffectNoUpdate.
        void ApplyEffectRemove(Game::MobEffectId id);
        // A fresh world: nothing carries over (MC builds a new LocalPlayer).
        void ClearEffects() { activeEffects.clear(); spinningEffectTime = spinningEffectSpeed = 0.0f; }
        // MC LocalPlayer.tickSpinningEffect / getSpinningEffectAngle — the
        // nausea warp's rotation (portal intensity is not modelled here:
        // the nether portal's own overlay is separate, so only NAUSEA spins).
        // MC Entity.tickCount as the effect code reads it (the darkness
        // pulse): counted in TickEffects.
        int   tickCount = 0;
        float spinningEffectTime  = 0.0f;
        float spinningEffectSpeed = 0.0f;
        float GetSpinningEffectAngle(float partialTick) const {
            return spinningEffectTime + partialTick * spinningEffectSpeed;
        }
        // MAX_HEALTH with HEALTH_BOOST — the HUD's heart rows.
        float GetMaxHealth() const;
        // Player.getDestroySpeed's effect factor (HASTE / CONDUIT_POWER /
        // MINING_FATIGUE) for the dig-progress machine.
        float GetEffectDigSpeedMultiplier() const { return Game::GetEffectDigSpeedMultiplier(activeEffects); }
        // Player.isMobilityRestricted — BLINDNESS: no sprinting.
        bool IsMobilityRestricted() const { return HasEffect(Game::MobEffectId::Blindness); }
        // The effect state the physics step reads (PlayerPhysics::effect*).
        void ApplyEffectPhysics();

        // ── Bed / sleeping ──────────────────────────────────────────────
        // MC LivingEntity.sleepingPos (its presence IS isSleeping) and
        // Player.sleepCounter. Both server-driven: PlayerSleepS2C sets and
        // clears the position; the counter then runs MC Player.tick's clock
        // locally (0→100 while asleep, 100→110→0 after waking), which is
        // what the HUD's sleep fade reads. The eye height while asleep is
        // Entity.SLEEPING_DIMENSIONS' 0.2.
        std::optional<glm::ivec3> sleepingPos;
        int  sleepCounter = 0;
        bool IsSleeping() const { return sleepingPos.has_value(); }
        float stepHeight = 0.6f;   // How high the player can step up

        // Game mode + abilities — synced from the server via
        // PlayerAbilitiesS2C (handlePlayerAbilities). gameMode holds the
        // Server::GameMode raw value (0 survival, 1 creative, 2 adventure,
        // 3 spectator). physics.mayFly / physics.isFlying carry the flight
        // half so the physics step can read them without reaching back here.
        uint8_t gameMode     = 0;
        // False until the first PlayerAbilitiesS2C lands. MC can't observe
        // this window — Minecraft.gameMode is null until handleLogin, and the
        // Gui needs a level to render, so the mode is always known by the time
        // anything reads it. Our render loop starts as soon as the socket
        // connects, so callers that would otherwise act on the survival
        // default (notably the HUD's hearts/hunger block) must check this
        // first, or a creative player sees a flash of survival UI on join.
        bool    gameModeKnown = false;
        bool    invulnerable = false;
        bool    instabuild   = false;
        float   flyingSpeed  = 0.05f;  // MC Abilities.flyingSpeed (per-tick)

        bool IsCreative()  const { return gameMode == 1; }
        bool IsSpectator() const { return gameMode == 3; }

        // === Predicted item-use state ===
        // Client-side mirror of the server's hold-to-use lifecycle
        // (LivingEntity.useItem / useItemRemaining) — MC's client runs
        // startUsingItem locally when MultiPlayerGameMode.useItem succeeds,
        // and we do the same when the RMB press starts a use (held stack has
        // GetUseDuration() > 0). Drives the viewmodel eat/drink/block pose;
        // the server remains authoritative for the actual consume (its
        // InventorySetSlotS2C corrects any mispredict).
        bool             usingItem         = false;
        int              useItemRemaining  = 0;   // ticks left (counts down at 20 TPS)
        int              useItemDuration   = 0;   // total ticks (for pose progress)
        uint32_t         usingHand         = 0;   // 0 = main, 1 = off
        ItemUseAnimation useAnim           = ItemUseAnimation::NONE;
        
        // === Inventory ===
        Inventory inventory;
        
        // === Raycast Cache ===
        std::optional<RaycastHit> lastBlockHit;  // Cached result from per-frame raycast
        // Immersive portals: the level the hit block is in, and the portal
        // the ray went through to reach it (0 = a plain hit in the player's
        // own level). The controller binds that level while it digs and
        // places, and the packets carry the dimension.
        Game::DimensionId lastBlockHitDimension = Game::DimensionId::Overworld;
        uint32_t          lastBlockHitPortalId  = 0;
        // Last camera-space forward vector — refreshed every frame by
        // UpdateRaycast(camera). Use this instead of yaw/pitch fields
        // when you need the live look direction; the yaw/pitch members
        // are stale because mouse-look writes camera.yaw/pitch directly
        // and only syncs back at teleport-style events.
        glm::vec3 lookDir{0.0f, 0.0f, 1.0f};
        // TODO: Add entity hit cache when entity system is implemented
        // std::optional<EntityHit> lastEntityHit;
        
        // === Input State ===
        glm::vec3 movementInput{0.0f};
        bool jumpPressed = false;
        bool jumpHeld = false;         // True while space is held (for water bobbing)
        bool sprintPressed = false;
        // MC Player.lastItemInMainHand — see the reset in Tick().
        uint32_t m_lastItemInMainHand = 0;

        // MC Player.attackStrengthTicker, mirrored client-side. The attack
        // indicator IS this value — MC's bar is drawn from the client's own
        // copy, and the server keeps an identical one so the damage it applies
        // matches what the bar showed.
        int attackStrengthTicker = 0;

        // MC Player.itemSwapTicker — the SECOND cooldown ticker, and the reason
        // a slow weapon is slow to raise as well as slow to swing. It counts
        // against the same delay but is reset only by a change of held item
        // (Player.resetAttackStrengthTicker resets both; onAttack →
        // resetOnlyAttackStrengthTicker resets just the attack one), so
        // swapping to an axe raises it over the axe's full 20-tick delay while
        // attacking with it leaves the hand where it is.
        int itemSwapTicker = 0;

        // ── Camera damage tilt / death spin (MC GameRenderer.bobHurt) ──────
        //
        // MC LivingEntity.animateHurt sets hurtDuration = hurtTime = 10 when a
        // hurt-animation packet arrives, and baseTick counts hurtTime down.
        // hurtDir is the attacker's bearing RELATIVE to our own yaw, which is
        // why the tilt leans away from the blow instead of always the same way.
        int   hurtTime     = 0;
        int   hurtDuration = 10;
        float hurtDir      = 0.0f;
        // MC LivingEntity.tickDeath — counts up to 20 while dead and drives the
        // camera roll toward 40 degrees.
        int   deathTime    = 0;

        bool sneakPressed = false;

        // Double-tap-space flight toggle state (MC jumpTriggerTime = 7 ticks).
        static constexpr float FLY_DOUBLE_TAP_WINDOW = 0.35f;
        bool  jumpKeyWasDown = false;  // Raw held state last frame (true edge detect)
        float flyToggleTimer = 0.0f;   // Seconds left in the double-tap window

        // Jump happened since the last PlayerMoveC2S send (jump exhaustion).
        // Set in UpdatePhysics, consumed by the move-send in PlatformMain.
        bool jumpedSinceMoveSend = false;

        // Largest fall-landing distance since the last PlayerMoveC2S send
        // (client physics tracks exact ground contact — see
        // PlayerPhysics::fallDistance). Consumed by the move-send; the
        // server turns it into fall damage.
        float landedFallSinceMoveSend = 0.0f;
        // The same landing, for the landing SOUND (LivingEntity
        // .causeFallDamage's playSound half) — consumed once per client tick
        // by Client::LocalPlayerSounds, independently of the move send
        // (mutable: the sound tick reads the player through a const view).
        mutable float landedFallForSound = 0.0f;
        
        // === Statistics ===
        PlayerStats stats;
        
        // === Public Methods ===
        
        // Initialize player state
        void Initialize();
        
        // MC Player.tick — the 20 Hz half of the local player's update. Call it
        // from the client tick loop, never from the frame loop (UpdatePhysics
        // is the per-frame half).
        void Tick();
        // MC LivingEntity.tickEffects (client branch) + LocalPlayer
        // .tickSpinningEffect: run from Tick().
        void TickEffects();

        // MC Player.getCurrentItemAttackStrengthDelay — 20 / ATTACK_SPEED, in
        // ticks. The player's base speed is 4.0, so a bare hand is 5 ticks and
        // a netherite axe (modifier -3.0) is 20.
        float GetCurrentItemAttackStrengthDelay() const;

        // MC Player.getAttackStrengthScale / getItemSwapScale, both clamped to
        // [0,1]. `adjust` is the fraction of a tick to add: the HUD passes 0.0,
        // the held-item renderer 1.0, the damage calculation (server side) 0.5.
        float GetAttackStrengthScale(float adjust) const;
        float GetItemSwapScale(float adjust) const;

        // Update physics simulation (accepts any IBlockAccess: World*, ClientBlockAccess*, etc.)
        void UpdatePhysics(float deltaTime, IBlockAccess* blockAccess);
        
        // Update raycast from camera position
        void UpdateRaycast(const Render::Camera& camera);
        
        // Update visual smoothing (lerp visual toward predicted)
        void UpdateVisual(float deltaTime);
        
        // Apply server position correction
        void ApplyServerCorrection(const glm::dvec3& pos, float newYaw, float newPitch);
        
        // Get current eye position (for camera)
        // DOUBLE: this is the camera's position. Returned as a float it put
        // the eye on a 3 cm grid at x = 300,000 — the world (exact, camera-
        // relative) shook against it with every step.
        glm::dvec3 GetEyePosition() const;
        
        // Get current eye height based on pose
        float GetEyeHeight() const;
        
        // Movement input setters
        void SetMovementInput(const glm::vec3& movement) { movementInput = movement; }
        void SetJumpPressed(bool pressed);
        void SetJumpHeld(bool held) { jumpHeld = held; }
        void SetSprintPressed(bool pressed) { sprintPressed = pressed; }
        void SetSneakPressed(bool pressed) { sneakPressed = pressed; }
        
        // Noclip control
        void ToggleNoclip();
        void SetNoclip(bool enabled);

        // Raised whenever a PlayerAbilitiesS2C wrote isFlying / noclip, and
        // consumed by ClientPlayerController's dirty check.
        //
        // Without it the client ECHOES the server's own value straight back as
        // if it were a local toggle, and on join that echo is stale. The join
        // sequence sends two abilities packets — a speculative one at login
        // (derived from the world's default game mode, so flying=false and
        // noclip=false) and the real one from playerdata a moment later. The
        // first flips the local flag off, the dirty check fires, and the client
        // tells the server "noclip=false" — which the server applies to the
        // ServerPlayer it had just restored to TRUE. If that C2S lands after
        // the server sent the real state, the server keeps `false`, saves
        // `false`, and the next rejoin drops the player out of the sky.
        bool abilitiesSyncedFromServer = false;
        
        // Inventory management
        void SelectSlot(int slot);
        void SelectNextSlot();
        void SelectPreviousSlot();
        BlockID GetSelectedBlock() const { return inventory.GetSelectedBlock(); }
        int GetSelectedSlot() const { return inventory.GetSelectedSlot(); }
        
        // Statistics tracking
        void UpdateStatistics(float deltaTime);
        const PlayerStats& GetStats() const { return stats; }
        
    private:
        // Track last position for distance calculations
        glm::dvec3 lastPosition{0.0};
        
        // Smoothing parameters
        static constexpr float POSITION_SMOOTHING_FACTOR = 10.0f;  // How fast visual lerps to predicted
        static constexpr float ROTATION_SMOOTHING_FACTOR = 15.0f;  // How fast rotation lerps
    };

} // namespace Game

namespace Client {
    // MC LivingEntity.tickEffects' client particle roll for every PLAYER this
    // client draws: the local one from its own effect list, the remote ones
    // from the synched visuals their PlayerUpdateS2C carries. (Mobs roll
    // their own in LivingEntity::TickEffects.) Once per client tick.
    void TickPlayerEffectParticles(const Game::ClientPlayer& localPlayer);
}
