// File: src/client/entity/RemotePlayerManager.hpp
#pragma once

#include "common/world/level/DimensionId.hpp"
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "common/portal/ImmersivePortal.hpp"
#endif

#include "common/core/Mth.hpp"

#include "common/entity/PlayerColors.hpp"
#include <glm/glm.hpp>
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
        bool positionInitialized = false;  // True after first UpdatePlayer call

        // Current rendered state (interpolated each tick)
        glm::vec3 position{0.0f};
        glm::vec2 rotation{0.0f}; // head yaw, pitch
        bool isCrouching = false;

        // Body yaw — follows movement direction or head with 50-degree max offset
        // (Minecraft's LivingEntity.yBodyRot)
        float bodyYaw = 0.0f;
        glm::vec3 prevPosition{0.0f}; // previous tick position for velocity estimation

        // Interpolation target (set when server packet arrives)
        glm::vec3 targetPosition{0.0f};
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
        glm::vec3 renderPrevPosition{0.0f};
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

        void SetDimension(uint32_t id, Game::DimensionId dimension) {
            auto it = m_players.find(id);
            if (it != m_players.end()) it->second.dimension = dimension;
        }

        void SetScale(uint32_t id, float scale) {
            m_players[id].scale = scale;   // the update that follows fills the rest
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
                                               glm::vec3& pos, glm::vec2& rot) {
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
            const glm::vec3 here(rp.pending.back.TransformPoint(glm::dvec3(pos)));
            // Well past the surface on the far side: the copy would never
            // catch up (a teleport there, a sprint through) — go now.
            if (rp.pending.forward.SignedDistanceToPlane(glm::dvec3(here)) < -4.0 * std::max(rp.scale, 0.05f)) {
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
            auto mapPoint = [&](const glm::vec3& p) {
                return glm::vec3(portal.TransformPoint(glm::dvec3(p)));
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
        void UpdatePlayer(uint32_t id, const glm::vec3& pos, const glm::vec2& rot, bool crouching,
                          Game::DimensionId dimension) {
            auto& rp = m_players[id];
            if (rp.positionInitialized) {
                const glm::vec3 jump = pos - rp.targetPosition;
                const float horizSq = jump.x * jump.x + jump.z * jump.z;
                // 3 m sideways is ten times a sprint's per-tick step; 10 m
                // up or down is over terminal velocity's. A scaled body
                // moves that much faster, so the limits scale with it.
                const float s = std::max(rp.scale, 0.05f);
                const bool teleported = dimension != rp.dimension ||
                                        horizSq > (3.0f * s) * (3.0f * s) || std::abs(jump.y) > 10.0f * s;
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

                // --- Position/rotation interpolation (Minecraft's InterpolationHandler) ---
                if (rp.lerpSteps > 0) {
                    float alpha = 1.0f / static_cast<float>(rp.lerpSteps);
                    rp.position = glm::mix(rp.position, rp.targetPosition, alpha);

                    float yawDiff = Wrap180(rp.targetRotation.x - rp.rotation.x);
                    rp.rotation.x += yawDiff * alpha;
                    rp.rotation.y = glm::mix(rp.rotation.y, rp.targetRotation.y, alpha);

                    rp.lerpSteps--;
                }

                // --- Body rotation (Minecraft's LivingEntity.tickHeadTurn) ---
                // Estimate horizontal velocity from position change
                glm::vec3 vel = rp.position - rp.prevPosition;
                float speedSq = vel.x * vel.x + vel.z * vel.z;
                rp.prevPosition = rp.position;

                float headYaw = rp.rotation.x;

                // Determine body target: movement direction when moving, head when still
                float bodyTarget;
                if (speedSq > 0.0001f) {
                    bodyTarget = Game::Mth::YRotFromVector(vel);
                } else {
                    bodyTarget = headYaw;
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
                    const double depth = rp.pending.forward.SignedDistanceToPlane(glm::dvec3(rp.position));
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
        void Clear() { m_players.clear(); }
        const std::unordered_map<uint32_t, RemotePlayer>& GetPlayers() const { return m_players; }

    private:
        std::unordered_map<uint32_t, RemotePlayer> m_players;
    };

    extern std::unique_ptr<RemotePlayerManager> g_remotePlayerManager;

} // namespace Client
