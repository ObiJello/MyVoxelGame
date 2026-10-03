// File: src/client/entity/RemotePlayerManager.hpp
#pragma once

#include "common/world/level/DimensionId.hpp"
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "common/portal/ImmersivePortal.hpp"
#endif

#include "common/core/Mth.hpp"

#include "common/entity/PlayerColors.hpp"
#include "common/entity/LivingEntity.hpp"   // WalkAnimationState (morph)
#include "common/entity/Morph.hpp"
#include "common/entity/ElytraAnimationState.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/Item.hpp"
#include <glm/glm.hpp>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <memory>
#include <string>
#include <cstdint>
#include <cmath>
#include <cctype>

#include <algorithm>

namespace Client {

    // Wrap angle to [-180, 180] range. Mirrors MC `Mth.wrapDegrees`
    // (Mth.java:221-232). Free function (not a class member) so the renderer
    // can use it for sub-tick rotation interpolation without dragging in
    // RemotePlayerManager's public surface.
    inline float Wrap180(float deg) {
        deg = fmodf(deg + 180.0f, 360.0f);
        if (deg < 0.0f) deg += 360.0f;
        return deg - 180.0f;
    }

    // MC `Mth.rotLerp(a, from, to)` (Mth.java:588-594) — linear lerp that
    // takes the SHORT way around the 360° circle. 350° → 10° lerps via
    // wrapDegrees(10°-350°) = wrapDegrees(-340°) = +20°, so the rotation
    // moves +20° not -340°.
    inline float RotLerp(float a, float from, float to) {
        return from + a * Wrap180(to - from);
    }

    struct RemotePlayer {
        uint32_t playerId = 0;
        std::string name;  // Player name (populated from PlayerInfoS2C ADD action)
        // Which level this player stands in — the scope of their last
        // position update. The list itself is global (a player exists once
        // regardless of where they are); renderers filter on this.
        Game::DimensionId dimension = Game::DimensionId::Overworld;
        // Stick-figure colour (server-broadcast in PlayerInfoS2C ADD). Default
        // = the historical neon green so unknown / pre-update servers behave
        // exactly as they did before colours existed.
        Game::PlayerColorId color = Game::PlayerColorId::Default;
        // Body size (server-broadcast on every position update): the stick
        // figure, its name tag and its culling box all take it.
        float scale = 1.0f;
        // MC Attributes.NAME_TAG_DISTANCE of that player (PlayerUpdateS2C):
        // how far away its name tag still shows.
        float nameTagDistance = 64.0f;
        // /invisible on the server: no body, no name tag, no chat bubble,
        // no portal ghost — the copy is tracked but never drawn.
        bool invisible = false;
        // The effect swirl and flags (MC DATA_EFFECT_PARTICLES, shared flags
        // 5 / 6), from every PlayerUpdateS2C.
        Game::EffectVisuals effects;
        // /morph on the server: this copy is drawn as that body — a mob or
        // block by the mob renderer, an item or orb by theirs (Game::Morph
        // code, kNone = none) — no stick figure, no name tag, no ghost.
        uint32_t morph = Game::Morph::kNone;
        // The morph's look beyond the code (Game::Morph::DefaultVariantOf —
        // a tropical fish's packed variant, a salmon's size).
        int32_t  morphVariant = 0;
        bool IsMorphed() const { return !Game::Morph::IsNone(morph); }
        // The game mode from PlayerInfoS2C (Server::GameMode raw value). A
        // spectator is never picked by the crosshair (MC Player.isPickable),
        // and is drawn — to a spectator, the only viewer the server does not
        // mark them invisible for — as a translucent floating head
        // (AvatarRenderer / PlayerModel with isSpectator).
        uint8_t gameMode = 0;
        bool IsSpectator() const { return gameMode == 3; }
        // MC LivingEntity.walkAnimation, driven from the tick's travel in
        // Tick(): the morph's limb swing (the stick figure has none).
        Game::WalkAnimationState walk;
        int ticks = 0;   // age since first seen, the mob models' idle clock
        // The morph's animation byte from the player (creeper swell), and
        // the previous tick's for the renderer's lerp.
        uint8_t morphAnim = 0;
        uint8_t morphAnimOld = 0;
        bool positionInitialized = false;  // True after first UpdatePlayer call

        // Current rendered state (interpolated each tick). World positions
        // are DOUBLE (camera-relative rendering, RenderOrigin.hpp): a float
        // sits on a 3 cm grid at x = 300,000, and the renderer subtracts
        // the render origin in double before anything reaches the GPU.
        glm::dvec3 position{0.0};
        glm::vec2 rotation{0.0f}; // head yaw, pitch
        bool isCrouching = false;

        // Body yaw — follows movement direction or head with 50-degree max offset
        // (Minecraft's LivingEntity.yBodyRot)
        float bodyYaw = 0.0f;
        glm::dvec3 prevPosition{0.0}; // previous tick position for velocity estimation

        // Interpolation target (set when server packet arrives)
        glm::dvec3 targetPosition{0.0};
        glm::vec2 targetRotation{0.0f};
        int lerpSteps = 0;

        // ── Previous-tick snapshot for SUB-TICK render interpolation ────────
        // Mirrors MC Entity.xo/yo/zo + yRotO/xRotO + yBodyRotO. Updated at the
        // START of RemotePlayerManager::Tick() — BEFORE the per-tick lerp step
        // writes the new "current" values to position/rotation/bodyYaw. The
        // renderer then lerps prev → current using a per-frame partialTick
        // fraction so frames within a tick show a continuously-advancing
        // position instead of a stair-step (Entity.java:1955-1960 for pos,
        // :1918 for yaw, :1914 for pitch).
        glm::dvec3 renderPrevPosition{0.0};
        glm::vec2 renderPrevRotation{0.0f};
        float     renderPrevBodyYaw = 0.0f;

        // MC LivingEntity.hurtTime — counts down from 10 and drives the red
        // flash. Server-set on the hit, then ticked down locally so the flash
        // is smooth between the 20 Hz position broadcasts.
        int hurtTime = 0;

        // MC LivingEntity.deathTime — 0..20, drives the corpse's topple. Sent
        // outright rather than max()'d like hurtTime: a respawn sends 0, and
        // taking the max would leave the revived player lying on the ground
        // forever. Advanced locally between broadcasts so the fall is smooth.
        int deathTime = 0;
        // MC Entity.isSprinting from PlayerUpdateS2C: drives the sprint dust
        // under this player (ParticleTicks::TickPlayers).
        bool sprinting = false;
        // MC LivingEntity.isFallFlying from PlayerUpdateS2C: gliding on an
        // elytra (where an attached firework rocket rides).
        bool fallFlying = false;
        // The worn elytra (PlayerUpdateS2C elytraFlags: kElytraWorn /
        // kElytraGlint) and its wings' animation (MC LivingEntity.
        // elytraAnimationState, ticked in Tick from this copy's travel).
        uint8_t elytraFlags = 0;
        Game::ElytraAnimationState elytraAnim;
        // MC LivingEntity.fallFlyTicks as this client counts it (one per
        // client tick while the glide flag is set): eases the glide pose in.
        int  fallFlyTicks = 0;

        // MC LivingEntity.sleepingPos — the bed's head cell while this player
        // is in it (PlayerSleepS2C). The renderer lays the figure down along
        // the bed (LivingEntityRenderer.setupRotations' SLEEPING branch) and
        // reads the bed's facing off the block under this position.
        std::optional<glm::ivec3> sleepingPos;

        // The entity this player sits on (PlayerMountS2C), 0 when none —
        // the renderer draws the sitting pose (HumanoidModel.setupAnim's
        // isPassenger legs and arms). The position already is the seat's.
        int32_t vehicleId = 0;

        // MC LivingEntity.swinging / swingTime / attackAnim / oAttackAnim —
        // started by PlayerSwingS2C (swing), advanced in Tick
        // (updateSwingTime), read as AttackAnim(partialTick). Poses a
        // morphed player's humanoid arm.
        bool  swinging    = false;
        int   swingTime   = 0;
        uint8_t swingHand = 0;
        // The swing's SwingAnimation duration (the main hand's attack
        // animation when it began: a spear's STAB, else the 6-tick WHACK).
        int   swingDuration = 6;
        // MC LivingEntity isAutoSpinAttack (the synched flag, PlayerUpdateS2C):
        // a riptide in flight — the spinning body and the swirl around it.
        bool  autoSpinAttack = false;
        // This copy's tick clock, and the tick of the last kinetic hit
        // (entity event 2) — the held spear's recoil.
        int   tickCount = 0;
        int   lastKineticHitFeedbackTick = -1000000;
        float TicksSinceKineticHitFeedback(float partialTick) const {
            if (lastKineticHitFeedbackTick <= -1000000) return 0.0f;
            return static_cast<float>(tickCount - lastKineticHitFeedbackTick) + partialTick;
        }
        float attackAnim  = 0.0f;
        float attackAnimO = 0.0f;
        // MC LivingEntity.getAttackAnim(partialTick).
        float AttackAnim(float partialTick) const {
            float diff = attackAnim - attackAnimO;
            if (diff < 0.0f) diff += 1.0f;
            return attackAnimO + diff * partialTick;
        }
        // AvatarRenderer.getArmPose and the item-use state, from every
        // PlayerUpdateS2C (PlayerArmPose.hpp ordinals).
        uint8_t  rightArmPose = 0;
        uint8_t  leftArmPose  = 0;
        bool     usingItem    = false;
        uint8_t  useItemHand  = 0;
        uint32_t ticksUsingItem = 0;
        uint8_t  maxCrossbowCharge = 25;

        // Chat bubble
        std::string chatBubbleText;
        float chatBubbleTimer = 0.0f;
        static constexpr float CHAT_BUBBLE_DURATION = 5.0f;

#if ENABLE_IMMERSIVE_PORTALS
        // A crossing the server has reported that this copy has not made
        // yet. The copy lags the real player by a few ticks, so when the
        // server files them in the far level the copy is still a step
        // short of the surface. It stays filed HERE, its far-side updates
        // mapped back through the portal, until its whole body has gone
        // through the plane — drawn the whole way by the main pass (cut at
        // the surface) and the portal view's crossers pass. Only then is
        // everything mapped through and the copy filed in the far level.
        struct PendingCrossing {
            bool                    active = false;
            Game::Immersive::Portal forward;   // this level -> the far one
            Game::Immersive::Portal back;      // the far one -> this level
            Game::DimensionId       to = Game::DimensionId::Overworld;
        };
        PendingCrossing pending;
#endif
    };

    class RemotePlayerManager {
    public:
        // The hurt flash arrives on the position broadcast; taking the MAX
        // stops a stale packet cutting a flash short when two arrive close
        // together.
        void SetHurtTime(uint32_t id, int hurtTime) {
            auto it = m_players.find(id);
            if (it != m_players.end()) {
                it->second.hurtTime = std::max(it->second.hurtTime, hurtTime);
            }
        }

        void SetDeathTime(uint32_t id, int deathTime) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.deathTime = deathTime;
        }
        void SetSprinting(uint32_t id, bool sprinting) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.sprinting = sprinting;
        }
        void SetElytraFlags(uint32_t id, uint8_t flags) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.elytraFlags = flags;
        }
        // The glide flag only; Tick counts fallFlyTicks (MC LivingEntity.tick
        // on the client), so the glide's ease-in runs at 20 Hz however the
        // broadcasts bunch up.
        void SetFallFlying(uint32_t id, bool fallFlying) {
            auto it = m_players.find(id);
            if (it == m_players.end()) return;
            it->second.fallFlying = fallFlying;
            if (!fallFlying) it->second.fallFlyTicks = 0;
        }

        // A player lay down (bed head cell) or got up (nullopt). Applied to
        // a known copy only — the sleep packet can precede PlayerInfo ADD
        // for a late joiner, and a copy that does not exist yet cannot be in
        // a bed.
        void SetSleepingPos(uint32_t id, const std::optional<glm::ivec3>& bedPos) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.sleepingPos = bedPos;
        }

        // A player sat on (vehicle id) / got off (0) an entity. Applied to a
        // known copy only, like the sleep: the join sends PlayerInfo first.
        void SetVehicle(uint32_t id, int32_t vehicleId) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.vehicleId = vehicleId;
        }

        // MC LivingEntity.getCurrentSwingDuration for a player with no HASTE /
        // MINING_FATIGUE (their levels are not synced for other players).
        static constexpr int kSwingDuration = 6;
        // ItemStack.getAttackAnimation().duration() (SpearItem.cpp).
        static int SwingDurationOf(Game::ItemID item);

        // MC LivingEntity.swing(hand, updateSelf): restarts unless a swing is
        // in its first half.
        void StartSwing(uint32_t id, uint8_t hand) {
            auto it = m_players.find(id);
            if (it == m_players.end()) return;
            RemotePlayer& rp = it->second;
            if (!rp.swinging || rp.swingTime >= rp.swingDuration / 2 || rp.swingTime < 0) {
                rp.swingTime = -1;
                rp.swinging  = true;
                rp.swingHand = hand;
                // The hand's getAttackAnimation: a spear STABs for its own
                // attack duration.
                int duration = kSwingDuration;
                if (const Equipment* eq = GetEquipment(id)) {
                    const Game::ItemStack& held = (*eq)[static_cast<size_t>(
                        hand == 1 ? Game::EquipmentSlot::OFFHAND : Game::EquipmentSlot::MAINHAND)];
                    if (!held.IsEmpty()) duration = SwingDurationOf(held.itemId);
                }
                rp.swingDuration = duration > 0 ? duration : kSwingDuration;
            }
        }

        // The synched riptide flag (PlayerUpdateS2C).
        void SetAutoSpinAttack(uint32_t id, bool spinning) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.autoSpinAttack = spinning;
        }

        // Entity event 2 on another player (LivingEntity.onKineticHit): at
        // most once per HIT_FEEDBACK_TICKS; true when it took (the caller
        // plays the hit sound).
        bool OnKineticHit(uint32_t id) {
            auto it = m_players.find(id);
            if (it == m_players.end()) return false;
            RemotePlayer& rp = it->second;
            if (rp.tickCount - rp.lastKineticHitFeedbackTick <= 10) return false;
            rp.lastKineticHitFeedbackTick = rp.tickCount;
            return true;
        }

        // The arm poses and use state carried by PlayerUpdateS2C.
        void SetArmPoses(uint32_t id, uint8_t right, uint8_t left, bool usingItem, uint8_t useHand,
                         uint32_t ticksUsing, uint8_t maxCrossbowCharge = 25) {
            auto it = m_players.find(id);
            if (it == m_players.end()) return;
            it->second.maxCrossbowCharge = maxCrossbowCharge;
            it->second.rightArmPose   = right;
            it->second.leftArmPose    = left;
            it->second.usingItem      = usingItem;
            it->second.useItemHand    = useHand;
            it->second.ticksUsingItem = ticksUsing;
        }

        void SetDimension(uint32_t id, Game::DimensionId dimension) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.dimension = dimension;
        }

        void SetScale(uint32_t id, float scale) {
            m_players[id].scale = scale;   // the update that follows fills the rest
        }
        void SetNameTagDistance(uint32_t id, float distance) {
            m_players[id].nameTagDistance = distance;
        }
        void SetInvisible(uint32_t id, bool invisible) {
            m_players[id].invisible = invisible;
        }
        // The player's synched effect visuals (MC DATA_EFFECT_PARTICLES + the
        // invisible / glowing flags) — the swirl their body gives off and the
        // GLOWING tint.
        void SetEffectVisuals(uint32_t id, Game::EffectVisuals visuals) {
            m_players[id].effects = std::move(visuals);
        }
        void SetMorph(uint32_t id, uint32_t morph, int32_t variant) {
            m_players[id].morph = morph;
            m_players[id].morphVariant = variant;
        }
        // PlayerInfoS2C ADD / UPDATE_GAME_MODE (lazy-create, as the name).
        void SetGameMode(uint32_t id, uint8_t gameMode) {
            auto& rp = m_players[id];
            rp.playerId = id;
            rp.gameMode = gameMode;
        }
        void SetMorphAnim(uint32_t id, uint8_t anim) {
            m_players[id].morphAnim = anim;
        }

#if ENABLE_IMMERSIVE_PORTALS
        // The server reports the player through `portal` into `to`; the
        // copy keeps walking here until it is through (see PendingCrossing).
        void BeginPortalCrossing(uint32_t id, const Game::Immersive::Portal& portal,
                                 Game::DimensionId to) {
            auto it = m_players.find(id);
            if (it == m_players.end()) return;
            RemotePlayer& rp = it->second;
            rp.pending.active  = true;
            rp.pending.forward = portal;
            rp.pending.back    = portal.MakeReverse();
            rp.pending.to      = to;
        }

        // A position update while a crossing is pending: one from the far
        // level is mapped back here, and the update is filed under THIS
        // level. Returns the level to file it under. An update from a
        // third level, or one that has moved far from the surface on the
        // far side, commits the crossing first (the copy will snap).
        Game::DimensionId ApplyPendingCrossing(uint32_t id, Game::DimensionId dimension,
                                               glm::dvec3& pos, glm::vec2& rot) {
            auto it = m_players.find(id);
            if (it == m_players.end() || !it->second.pending.active) return dimension;
            RemotePlayer& rp = it->second;
            if (dimension == rp.dimension) {           // they came back out
                rp.pending.active = false;
                return dimension;
            }
            if (dimension != rp.pending.to) {          // somewhere else entirely
                CommitPortalCrossing(rp);
                return dimension;
            }
            const glm::dvec3 here = rp.pending.back.TransformPoint(pos);
            // Well past the surface on the far side: the copy would never
            // catch up (a teleport there, a sprint through) — go now.
            if (rp.pending.forward.SignedDistanceToPlane(here) < -4.0 * std::max(rp.scale, 0.05f)) {
                CommitPortalCrossing(rp);
                return dimension;
            }
            pos = here;
            const glm::vec3 look = Game::Mth::ViewVector(rot.y, rot.x);
            const glm::vec3 mapped(glm::normalize(rp.pending.back.TransformLocalVecNonScale(glm::dvec3(look))));
            rot = { Game::Mth::YRotFromVector(mapped), Game::Mth::XRotFromVector(mapped) };
            return rp.dimension;
        }

        // The player went through `portal` (the server's next position is
        // on its far side). Everything this copy is interpolating — where it
        // is, where it was, where it is heading, which way it faces — is
        // mapped through the portal, so the walk continues in the far
        // level's coordinates from exactly where it was. A snap to the
        // arrival point instead jumped the body forward by however far this
        // copy lagged the real one (up to three ticks): the "teleport hitch"
        // an observer saw at every nether portal.
        void MapThroughPortal(uint32_t id, const Game::Immersive::Portal& portal,
                              Game::DimensionId newDimension) {
            auto it = m_players.find(id);
            if (it == m_players.end() || !it->second.positionInitialized) return;
            RemotePlayer& rp = it->second;
            auto mapPoint = [&](const glm::dvec3& p) {
                return portal.TransformPoint(p);
            };
            auto mapYaw = [&](float yaw, float pitch, float& outYaw, float& outPitch) {
                const glm::vec3 look = Game::Mth::ViewVector(pitch, yaw);
                const glm::vec3 mapped(glm::normalize(portal.TransformLocalVecNonScale(glm::dvec3(look))));
                outYaw   = Game::Mth::YRotFromVector(mapped);
                outPitch = Game::Mth::XRotFromVector(mapped);
            };
            rp.position           = mapPoint(rp.position);
            rp.prevPosition       = mapPoint(rp.prevPosition);
            rp.targetPosition     = mapPoint(rp.targetPosition);
            rp.renderPrevPosition = mapPoint(rp.renderPrevPosition);
            float y, p;
            mapYaw(rp.rotation.x, rp.rotation.y, y, p);                     rp.rotation = {y, p};
            mapYaw(rp.targetRotation.x, rp.targetRotation.y, y, p);         rp.targetRotation = {y, p};
            mapYaw(rp.renderPrevRotation.x, rp.renderPrevRotation.y, y, p); rp.renderPrevRotation = {y, p};
            mapYaw(rp.bodyYaw, 0.0f, y, p);                                 rp.bodyYaw = y;
            mapYaw(rp.renderPrevBodyYaw, 0.0f, y, p);                       rp.renderPrevBodyYaw = y;
            rp.dimension = newDimension;
        }

        void CommitPortalCrossing(RemotePlayer& rp) {
            if (!rp.pending.active) return;
            rp.pending.active = false;
            MapThroughPortal(rp.playerId, rp.pending.forward, rp.pending.to);
        }
#endif

        // `dimension` is the level the update was scoped to. A player whose
        // level changed, or who moved farther than walking allows between
        // two broadcasts (a portal, /tp), is SNAPPED to the new spot: the
        // usual three-tick lerp would slide them across the world — and,
        // after a nether portal, across the far level from where the
        // Overworld coordinates happen to fall in it — for a visible
        // moment. MC removes and re-adds the entity on a level change for
        // the same reason.
        void UpdatePlayer(uint32_t id, const glm::dvec3& pos, const glm::vec2& rot, bool crouching,
                          Game::DimensionId dimension) {
            auto& rp = m_players[id];
            if (rp.positionInitialized) {
                const glm::dvec3 jump = pos - rp.targetPosition;
                const double horizSq = jump.x * jump.x + jump.z * jump.z;
                // 3 m sideways is ten times a sprint's per-tick step; 10 m
                // up or down is over terminal velocity's. A scaled body
                // moves that much faster, so the limits scale with it.
                const double s = std::max(static_cast<double>(rp.scale), 0.05);
                const bool teleported = dimension != rp.dimension ||
                                        horizSq > (3.0 * s) * (3.0 * s) || std::abs(jump.y) > 10.0 * s;
                if (teleported) rp.positionInitialized = false;
            }
            rp.dimension = dimension;
            if (!rp.positionInitialized) {
                // First position packet for this player — snap everything (PlayerInfo may have
                // already created the entry to set the name, so we can't use playerId == 0).
                rp.playerId = id;
                rp.position = pos;
                rp.rotation = rot;
                rp.targetPosition = pos;
                rp.targetRotation = rot;
                rp.bodyYaw = rot.x; // start body facing same as head
                rp.prevPosition = pos;
                // Seed the render-prev snapshot to the same spawn point — without
                // this, the first frame after spawn would lerp from origin (0,0,0)
                // up to the spawn position, briefly visualising the player at 0,0,0.
                rp.renderPrevPosition = pos;
                rp.renderPrevRotation = rot;
                rp.renderPrevBodyYaw  = rot.x;
                rp.lerpSteps = 0;
                rp.positionInitialized = true;
            } else {
                rp.targetPosition = pos;
                rp.targetRotation = rot;
                rp.lerpSteps = 3;
            }
            rp.isCrouching = crouching;
        }

        // Apply one interpolation step + body rotation. Call at 20Hz.
        void Tick() {
            for (auto& [id, rp] : m_players) {
                if (rp.hurtTime > 0) --rp.hurtTime;
                // MC LivingEntity.baseTick's oAttackAnim, then aiStep's
                // updateSwingTime.
                ++rp.tickCount;
                rp.attackAnimO = rp.attackAnim;
                if (rp.swinging) {
                    if (++rp.swingTime >= rp.swingDuration) {
                        rp.swingTime = 0;
                        rp.swinging  = false;
                    }
                } else {
                    rp.swingTime = 0;
                }
                rp.attackAnim = static_cast<float>(rp.swingTime) / static_cast<float>(rp.swingDuration);
                // MC LivingEntity.tickDeath, client-side: the corpse keeps
                // falling between the 10 Hz broadcasts that correct it.
                if (rp.deathTime > 0 && rp.deathTime < 20) ++rp.deathTime;
                // Snapshot what THIS tick is starting from — the renderer uses
                // these as the "previous" point for sub-tick interpolation.
                // Mirrors MC: LivingEntity.baseTick() updates yRotO/xRotO/
                // yHeadRotO/yBodyRotO at tick boundary; Entity.setOldPos() does
                // the same for xo/yo/zo. MUST happen BEFORE the per-tick lerp
                // below writes the new "current" values.
                rp.renderPrevPosition = rp.position;
                rp.renderPrevRotation = rp.rotation;
                rp.renderPrevBodyYaw  = rp.bodyYaw;
                rp.morphAnimOld       = rp.morphAnim;

                // --- Position/rotation interpolation (Minecraft's InterpolationHandler) ---
                if (rp.lerpSteps > 0) {
                    const double alpha = 1.0 / static_cast<double>(rp.lerpSteps);
                    rp.position = glm::mix(rp.position, rp.targetPosition, alpha);

                    const float alphaF = static_cast<float>(alpha);
                    float yawDiff = Wrap180(rp.targetRotation.x - rp.rotation.x);
                    rp.rotation.x += yawDiff * alphaF;
                    rp.rotation.y = glm::mix(rp.rotation.y, rp.targetRotation.y, alphaF);

                    rp.lerpSteps--;
                }

                // --- Body rotation (Minecraft's LivingEntity.tickHeadTurn) ---
                // Estimate horizontal velocity from position change. The
                // difference of two doubles; a per-tick step is a small
                // number, so its float copy below (direction only) is exact
                // enough anywhere in the world.
                const glm::dvec3 vel = rp.position - rp.prevPosition;
                const double speedSq = vel.x * vel.x + vel.z * vel.z;
                rp.prevPosition = rp.position;
                // MC LivingEntity.tick: fallFlyTicks counts the glide, then
                // elytraAnimationState.tick(): the wings follow the glide
                // (its dive from this tick's travel), the crouch, or fold.
                rp.fallFlyTicks = rp.fallFlying ? rp.fallFlyTicks + 1 : 0;
                rp.elytraAnim.Tick(rp.fallFlying, rp.isCrouching, vel);

                // MC RemotePlayer.tick → calculateEntityAnimation(false):
                // horizontal travel × 4, capped at 1, smoothed by 0.4 a tick
                // (updateWalkAnimation); a passenger or a corpse stops the
                // limbs outright (walkAnimation.stop). The skinned body and
                // a morph read it.
                ++rp.ticks;
                if (rp.vehicleId != 0 || rp.deathTime > 0) {
                    rp.walk.Stop();
                } else {
                    float f = static_cast<float>(std::sqrt(speedSq)) * 4.0f;
                    if (f > 1.0f) f = 1.0f;
                    rp.walk.Update(f, 0.4f, 1.0f);
                }

                float headYaw = rp.rotation.x;

                // Body target — MC LivingEntity.aiStep: the movement
                // direction while moving; standing still it is the body's
                // OWN yaw, i.e. the body stays put and only the ±50° clamp
                // below drags it after the head. It used to chase the head
                // whenever still, which left a mob morph's head never
                // turning against its body.
                float bodyTarget;
                if (speedSq > 0.0001) {
                    bodyTarget = Game::Mth::YRotFromVector(glm::vec3(vel));
                } else {
                    bodyTarget = rp.bodyYaw;
                }

                // Smooth body toward target at 30% per tick
                float bodyDiff = Wrap180(bodyTarget - rp.bodyYaw);
                rp.bodyYaw += bodyDiff * 0.3f;

                // Clamp: head can't rotate more than 50 degrees from body
                float headOffset = Wrap180(headYaw - rp.bodyYaw);
                if (fabsf(headOffset) > 50.0f) {
                    rp.bodyYaw += headOffset - copysignf(50.0f, headOffset);
                }

#if ENABLE_IMMERSIVE_PORTALS
                // A pending crossing completes once the whole body is past
                // the plane (the centre a body's half-width beyond it).
                if (rp.pending.active) {
                    const double depth = rp.pending.forward.SignedDistanceToPlane(rp.position);
                    const double halfWidth = 0.3 * std::max(rp.scale, 0.05f);
                    if (depth < -(halfWidth + 0.05)) CommitPortalCrossing(rp);
                }
#endif
            }
        }

        void SetChatBubble(uint32_t playerId, const std::string& message) {
            auto it = m_players.find(playerId);
            if (it != m_players.end()) {
                // Strip "<Name> " prefix to show just the message in the bubble
                std::string text = message;
                if (text.size() > 2 && text[0] == '<') {
                    auto closeAngle = text.find("> ");
                    if (closeAngle != std::string::npos) {
                        text = text.substr(closeAngle + 2);
                    }
                }
                it->second.chatBubbleText = text;
                it->second.chatBubbleTimer = RemotePlayer::CHAT_BUBBLE_DURATION;
            }
        }

        void UpdateBubbles(float deltaTime) {
            for (auto& [id, rp] : m_players) {
                if (rp.chatBubbleTimer > 0.0f) {
                    rp.chatBubbleTimer -= deltaTime;
                    if (rp.chatBubbleTimer <= 0.0f) {
                        rp.chatBubbleText.clear();
                        rp.chatBubbleTimer = 0.0f;
                    }
                }
            }
        }

        // Set the player's name. Creates an entry if the player isn't tracked yet so that
        // PlayerInfoS2C ADD can arrive before the first position update (matching MC, where
        // ClientboundPlayerInfoUpdatePacket arrives before the player entity is spawned).
        void SetPlayerName(uint32_t id, const std::string& name) {
            auto& rp = m_players[id];
            rp.playerId = id;
            rp.name = name;
        }

        // Set the player's stick-figure colour. Same lazy-create semantics as
        // SetPlayerName so the colour from PlayerInfoS2C ADD lands cleanly even
        // before the first position packet arrives.
        void SetPlayerColor(uint32_t id, Game::PlayerColorId color) {
            auto& rp = m_players[id];
            rp.playerId = id;
            rp.color = color;
        }

        // Case-insensitive name lookup (matching MC's PlayerList.getPlayerByName)
        const RemotePlayer* FindPlayerByName(const std::string& name) const {
            for (const auto& [id, rp] : m_players) {
                if (rp.name.size() == name.size()) {
                    bool equal = true;
                    for (size_t i = 0; i < name.size(); i++) {
                        if (std::tolower(static_cast<unsigned char>(rp.name[i])) !=
                            std::tolower(static_cast<unsigned char>(name[i]))) {
                            equal = false; break;
                        }
                    }
                    if (equal) return &rp;
                }
            }
            return nullptr;
        }

        void RemovePlayer(uint32_t id) { m_players.erase(id); }
        void Clear() { m_players.clear(); m_equipment.clear(); }

        // ── Equipment (MC ClientboundSetEquipmentPacket for a player) ─────
        // What each other player holds and wears (BodyArmorS2C keyed by the
        // player's id), by Game::EquipmentSlot ordinal — the stacks a
        // /morph body draws in its hands and on its armour layer. Kept
        // apart from the player copies: a copy dropped out of tracking
        // keeps its equipment (the server sends only changes); forgotten
        // when the player leaves (PlayerInfo REMOVE).
        static constexpr int kEquipmentSlots = 6;   // MAINHAND..HEAD
        using Equipment = std::array<Game::ItemStack, kEquipmentSlots>;
        void SetEquipment(uint32_t id, Game::EquipmentSlot slot, const Game::ItemStack& stack) {
            const int i = static_cast<int>(slot);
            if (i < 0 || i >= kEquipmentSlots) return;
            m_equipment[id][static_cast<size_t>(i)] = stack;
        }
        const Equipment* GetEquipment(uint32_t id) const {
            auto it = m_equipment.find(id);
            return it == m_equipment.end() ? nullptr : &it->second;
        }
        void ForgetEquipment(uint32_t id) { m_equipment.erase(id); }
        const std::unordered_map<uint32_t, RemotePlayer>& GetPlayers() const { return m_players; }
        // One player's copy to write (Client::Vehicles puts a riding player
        // in its seat after the entities tick), null when unknown.
        RemotePlayer* GetMutable(uint32_t id) {
            auto it = m_players.find(id);
            return it == m_players.end() ? nullptr : &it->second;
        }

    private:
        std::unordered_map<uint32_t, RemotePlayer> m_players;
        std::unordered_map<uint32_t, Equipment> m_equipment;
    };

    extern std::unique_ptr<RemotePlayerManager> g_remotePlayerManager;

} // namespace Client
