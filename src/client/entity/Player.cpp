// File: src/client/entity/Player.cpp
#include "Player.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/world/level/World.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/Log.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include "common/core/Features.hpp"
#include "client/world/ClientLevel.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#endif

namespace Game {

    ClientPlayer::ClientPlayer() {
        Initialize();
    }

    void ClientPlayer::Initialize() {
        // Initialize inventory with default blocks
        inventory.InitializeDefaults();

        // Initialize physics at a safe spawn position
        physics.position = glm::vec3(0.0f, 67.0f, 0.0f);
        physics.velocity = glm::vec3(0.0f);
        physics.isOnGround = false;
        physics.isSneaking = false;
        physics.isSprinting = false;
        physics.noclip = false;

        // Initialize transform tracking
        serverPos = glm::dvec3(physics.position);
        predictedPos = glm::dvec3(physics.position);
        visualPos = glm::dvec3(physics.position);
        
        lastPosition = physics.position;

        Log::Info("ClientPlayer initialized at position (%.2f, %.2f, %.2f)",
                  physics.position.x, physics.position.y, physics.position.z);
    }

    void ClientPlayer::Tick() {
        // MC Player.tick:257-266. These are the client's own copies of the
        // attack cooldown — the attack indicator IS attackStrengthTicker, and
        // the server keeps an identical one so the bar and the damage agree.
        //
        // They live in the 20 Hz tick, NOT in UpdatePhysics: that runs once per
        // FRAME, so counting there filled the bar at frame rate (three times
        // too fast at 60 fps) and the indicator read full while the server was
        // still charging.
        ++attackStrengthTicker;
        ++itemSwapTicker;

        // MC compares the ITEM (ItemStack.isSameItem), not the slot: moving
        // between two hotbar slots holding the same sword keeps the charge,
        // and a stack that changes under a stationary selection loses it.
        //
        // An item change resets BOTH tickers (Player.resetAttackStrengthTicker);
        // an ATTACK resets only the attack one (onAttack →
        // resetOnlyAttackStrengthTicker, done in the controller's attack path).
        const uint32_t heldNow = static_cast<uint32_t>(inventory.GetSelectedItem());
        if (heldNow != m_lastItemInMainHand) {
            m_lastItemInMainHand = heldNow;
            attackStrengthTicker = 0;
            itemSwapTicker = 0;
        }

        // MC LivingEntity.baseTick counts the hurt flash down; tickDeath counts
        // the death animation up and stops at 20 (where the camera roll's
        // asymptote leaves it).
        if (hurtTime > 0) --hurtTime;
        if (health <= 0) {
            if (deathTime < 20) ++deathTime;
        } else {
            deathTime = 0;
        }
    }

    float ClientPlayer::GetCurrentItemAttackStrengthDelay() const {
        float itemDamage = 0.0f, itemSpeed = 0.0f;
        Game::GetItemAttackAttributes(
            static_cast<uint32_t>(inventory.GetSelectedItem()), itemDamage, itemSpeed);
        const float attackSpeed = Game::kPlayerBaseAttackSpeed + itemSpeed;
        // A pathological modifier could zero this; a NaN delay would make every
        // swing read as fully charged. Mirrors ServerPlayer's guard.
        if (attackSpeed <= 0.0f) return 1.0e6f;
        return 20.0f / attackSpeed;
    }

    float ClientPlayer::GetAttackStrengthScale(float adjust) const {
        const float v = (static_cast<float>(attackStrengthTicker) + adjust)
                      / GetCurrentItemAttackStrengthDelay();
        return std::clamp(v, 0.0f, 1.0f);
    }

    float ClientPlayer::GetItemSwapScale(float adjust) const {
        const float v = (static_cast<float>(itemSwapTicker) + adjust)
                      / GetCurrentItemAttackStrengthDelay();
        return std::clamp(v, 0.0f, 1.0f);
    }

    void ClientPlayer::UpdatePhysics(float deltaTime, IBlockAccess* blockAccess) {
        // Update physics state based on input
        physics.isSneaking = sneakPressed;
        // Sneak normally blocks sprinting, but shift while flying is "descend",
        // not crouch — MC's canStartSprinting only rejects on isMovingSlowly()
        // → isCrouching(), and the crouch pose is never entered while
        // abilities.flying (LocalPlayer.java:1075, 622). Without the flying
        // exemption, sprint-descending would silently drop back to base speed.
        //
        // Noclip needs the SAME exemption and is a separate flag, not a kind of
        // isFlying. Missing it made sprint asymmetric in noclip: ascending
        // (jump) kept the boost because sneak was not held, descending (sneak)
        // silently lost it. Ascend and descend are the same gesture there, so
        // they must answer the same.
        physics.isSprinting =
            sprintPressed && (!sneakPressed || physics.isFlying || physics.noclip);

        // Create physics context with block access (World, ClientBlockAccess, etc.)
        PhysicsContext context;
        context.blockAccess = blockAccess;

        // In water, use held state so holding space continuously bobs upward.
        // On land, use edge-triggered jumpPressed for single jumps.
        bool jumpInput = physics.isInWater ? jumpHeld : jumpPressed;

        // Apply physics simulation with context.
        //
        // In steps of at most one tick. The frame delta is whatever the
        // clock says, and on macOS a window resize or a title-bar drag
        // blocks the event loop for as long as the mouse is down — the next
        // frame arrives with a delta of whole seconds. Integrated in one
        // go, gravity builds to a huge velocity and the position moves far
        // enough in a single step to land past the floor (the collision
        // snap only sees the start and end of the step), so a resize
        // dropped the player through the ground. A long pause is also
        // bounded to a quarter second of simulation: the world stood still
        // while the window was being dragged, so the player does too.
        constexpr float kMaxStep      = 0.05f;   // one tick
        constexpr float kMaxSimulated = 0.25f;
        float remaining = std::min(deltaTime, kMaxSimulated);
        bool  first = true;
        bool  jumped = false;   // the edge below reads it after the last step
        do {
            const float step = std::min(remaining, kMaxStep);
            // A jump is an edge, pressed once; only the first step sees it
            // (held-state water bobbing is repeated, like the key).
            const bool stepJump = jumpInput && (first || physics.isInWater);
            UpdatePlayerPhysics(physics, movementInput, stepJump, sneakPressed, step, context);
            jumped |= physics.didJumpThisStep;
            remaining -= step;
            first = false;
        } while (remaining > 1e-6f);
        physics.didJumpThisStep = jumped;

        // Accumulate jump impulses for the next PlayerMoveC2S (server-side
        // jump exhaustion — MC ServerPlayer.jumpFromGround). Cleared by the
        // move-send in PlatformMain each client tick.
        if (physics.didJumpThisStep) {
            jumpedSinceMoveSend = true;
        }

        // Flush this step's fall landing (if any) toward the next move
        // packet. Multiple landings inside one tick keep the largest.
        if (physics.landedFallDistance > 0.0f) {
            landedFallSinceMoveSend =
                std::max(landedFallSinceMoveSend, physics.landedFallDistance);
            physics.landedFallDistance = 0.0f;
        }

        // Update predicted position from physics
        predictedPos = glm::dvec3(physics.position);

        // Reset single-frame inputs
        jumpPressed = false;

        // Double-tap window countdown (MC decrements jumpTriggerTime per tick)
        if (flyToggleTimer > 0.0f) {
            flyToggleTimer -= deltaTime;
            if (flyToggleTimer < 0.0f) flyToggleTimer = 0.0f;
        }

        // Update mesh system with player position
        if (::Render::g_clientMeshManager) {
            ::Render::g_clientMeshManager->SetPlayerPosition(physics.position);
        }
    }

    void ClientPlayer::UpdateRaycast(const Render::Camera& camera) {
        // Calculate ray direction from camera
        const glm::vec3 front = camera.GetForward();

        // Cache the live look direction so consumers (e.g. portal-gun
        // projectile spawn) can use the camera-space forward without
        // having to re-derive it from stale yaw/pitch fields.
        lookDir = front;

        // Cast ray from camera position (player's eyes)
        const float kReach = 5.0f * physics.scale;   // default interaction range, with the body
        lastBlockHit = Raycast::CastRay(camera.position, front, kReach);
        lastBlockHitDimension = Client::ClientLevels::HasSession()
            ? Client::ClientLevels::ActiveDimension() : Game::DimensionId::Overworld;
        lastBlockHitPortalId  = 0;

#if ENABLE_IMMERSIVE_PORTALS
        // Through a portal: if the ray pierces a see-through surface before
        // it hits anything, the rest of it continues in the portal's
        // destination level, from the mapped point along the mapped
        // direction. One portal deep — reaching through two at once is
        // not something a hand does.
        if (Client::ClientLevels::HasSession()) {
            namespace PortalFlag = Game::Immersive::PortalFlag;
            const glm::dvec3 from(camera.position);
            // The segment runs a centimetre PAST the block hit: a wall-mounted
            // surface sits a thousandth of a block off the face the ray
            // stopped on, and the hit point must count as behind it.
            const float reachToBlock = lastBlockHit ? lastBlockHit->distance + 0.01f : kReach;
            const glm::dvec3 to = from + glm::dvec3(front) * static_cast<double>(reachToBlock);
            const Game::Immersive::Portal* best = nullptr;
            double bestT = 2.0;
            glm::dvec3 pierce{0.0};
            Client::GetClientImmersivePortals().ForEach([&](const Game::Immersive::Portal& p) {
                if (!p.Has(PortalFlag::Interactable) || !p.Has(PortalFlag::Visible)) return;
                // Same tolerance as a crossing: a gun portal counts as its
                // whole 1×2 opening, not just the oval.
                const auto hit = p.RaytraceSegment(from, to, p.CrossingLeniency());
                if (!hit || hit->t >= bestT) return;
                best = &p; bestT = hit->t; pierce = hit->point;
            });
            if (best) {
                const Game::Immersive::Portal portal = *best;   // copy: the level rebinds below
                const double travelled = glm::length(pierce - from);
                const float  remaining = kReach - static_cast<float>(travelled);
                const glm::dvec3 farOrigin = portal.TransformPoint(pierce);
                const glm::dvec3 farDir    = glm::normalize(portal.TransformLocalVecNonScale(glm::dvec3(front)));
                const Game::DimensionId dest = portal.IsMirror() ? portal.dimension : portal.destDimension;
                std::optional<RaycastHit> farHit;
                if (remaining > 0.0f) {
                    // WithLevel binds the far level's block access for the
                    // duration — the same access the ray reads.
                    Client::ClientLevels::WithLevel(dest, [&]() {
                        farHit = Raycast::CastRay(glm::vec3(farOrigin + farDir * 0.001),
                                                  glm::vec3(farDir), remaining);
                    });
                }
                if (farHit) farHit->distance += static_cast<float>(travelled);
                lastBlockHit          = farHit;   // nothing behind the surface counts
                lastBlockHitDimension = dest;
                lastBlockHitPortalId  = portal.id;
            }
        }
#endif
    }

    void ClientPlayer::UpdateVisual(float deltaTime) {
        // TODO: Implement smooth interpolation between server and predicted positions
        // For now, just copy predicted position directly (no smoothing)
        visualPos = predictedPos;
        visualYaw = yaw;
        visualPitch = pitch;

        // In the future, this would do something like:
        // visualPos = glm::mix(visualPos, predictedPos, 
        //                      1.0f - exp(-POSITION_SMOOTHING_FACTOR * deltaTime));
        // visualYaw = glm::mix(visualYaw, yaw,
        //                      1.0f - exp(-ROTATION_SMOOTHING_FACTOR * deltaTime));
        // visualPitch = glm::mix(visualPitch, pitch,
        //                        1.0f - exp(-ROTATION_SMOOTHING_FACTOR * deltaTime));
    }

    void ClientPlayer::ApplyServerCorrection(const glm::dvec3& pos, float newYaw, float newPitch) {
        // TODO: Implement server position correction with prediction reconciliation
        // For now, just accept the server position directly
        serverPos = pos;
        
        // Calculate prediction error
        glm::dvec3 error = serverPos - predictedPos;
        
        // If error is significant, snap to server position
        if (glm::length(error) > 0.1) {
            physics.position = glm::vec3(serverPos);
            predictedPos = serverPos;
            Log::Debug("Applied server correction: error was (%.3f, %.3f, %.3f)",
                      error.x, error.y, error.z);
        }
        
        // Update rotation
        yaw = newYaw;
        pitch = newPitch;
    }

    glm::vec3 ClientPlayer::GetEyePosition() const {
        return physics.GetEyePosition();
    }

    float ClientPlayer::GetEyeHeight() const {
        return physics.GetEyeHeight();
    }

    void ClientPlayer::SetJumpPressed(bool pressed) {
        // True key edge (jumpPressed below is consumed by physics each frame,
        // so it can't be used for tap detection — holding space re-registers).
        const bool risingEdge = pressed && !jumpKeyWasDown;
        jumpKeyWasDown = pressed;

        if (pressed && !jumpPressed) {
            jumpPressed = true; // Only register the press edge
        }

        // Double-tap-space creative flight toggle — MC LocalPlayer.aiStep
        // (LocalPlayer.java:760-782): first tap arms a 7-tick (0.35 s)
        // window; a second tap inside it flips abilities.flying. Gated on
        // mayFly; water bobbing and debug noclip keep their own controls.
        if (risingEdge && physics.mayFly && !physics.noclip &&
            (physics.isFlying || !physics.isInWater)) {
            if (flyToggleTimer > 0.0f) {
                physics.isFlying = !physics.isFlying;
                if (physics.isFlying) {
                    // Kill fall velocity so the toggle arrests the drop.
                    physics.velocity.y = 0.0f;
                }
                flyToggleTimer = 0.0f;
            } else {
                flyToggleTimer = FLY_DOUBLE_TAP_WINDOW;
            }
        }
    }

    void ClientPlayer::ToggleNoclip() {
        physics.noclip = !physics.noclip;
        Log::Info("Noclip %s", physics.noclip ? "enabled" : "disabled");

        if (physics.noclip) {
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
        }
    }

    void ClientPlayer::SetNoclip(bool enabled) {
        physics.noclip = enabled;
        Log::Info("Noclip %s", physics.noclip ? "enabled" : "disabled");

        if (physics.noclip) {
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
        }
    }

    void ClientPlayer::SelectSlot(int slot) {
        inventory.SetSelectedSlot(slot);
    }

    void ClientPlayer::SelectNextSlot() {
        inventory.SelectNextSlot();
    }

    void ClientPlayer::SelectPreviousSlot() {
        inventory.SelectPreviousSlot();
    }

    void ClientPlayer::UpdateStatistics(float deltaTime) {
        // Update play time
        stats.totalPlayTime += deltaTime;

        // Calculate distance traveled
        float distanceThisFrame = glm::length(physics.position - lastPosition);
        stats.totalDistanceTraveled += distanceThisFrame;
        lastPosition = physics.position;
    }

} // namespace Game