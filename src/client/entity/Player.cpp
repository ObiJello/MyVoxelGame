// File: src/client/entity/Player.cpp
#include "Player.hpp"
#include "common/world/level/World.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/Log.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include <glm/gtc/constants.hpp>
#include <algorithm>

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

    void ClientPlayer::UpdatePhysics(float deltaTime, IBlockAccess* blockAccess) {
        // Update physics state based on input
        physics.isSneaking = sneakPressed;
        // Sneak normally blocks sprinting, but shift while flying is "descend",
        // not crouch — MC's canStartSprinting only rejects on isMovingSlowly()
        // → isCrouching(), and the crouch pose is never entered while
        // abilities.flying (LocalPlayer.java:1075, 622). Without the flying
        // exemption, sprint-descending would silently drop back to base speed.
        physics.isSprinting = sprintPressed && (!sneakPressed || physics.isFlying);

        // Create physics context with block access (World, ClientBlockAccess, etc.)
        PhysicsContext context;
        context.blockAccess = blockAccess;

        // In water, use held state so holding space continuously bobs upward.
        // On land, use edge-triggered jumpPressed for single jumps.
        bool jumpInput = physics.isInWater ? jumpHeld : jumpPressed;

        // Apply physics simulation with context
        UpdatePlayerPhysics(physics, movementInput, jumpInput, sneakPressed, deltaTime, context);

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
        glm::vec3 front;
        front.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        front.y = sin(glm::radians(camera.pitch));
        front.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        front = glm::normalize(front);

        // Cache the live look direction so consumers (e.g. portal-gun
        // projectile spawn) can use the camera-space forward without
        // having to re-derive it from stale yaw/pitch fields.
        lookDir = front;

        // Cast ray from camera position (player's eyes)
        lastBlockHit = Raycast::CastRay(camera.position, front, 5.0f); // Using default interaction range
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
        return physics.isSneaking ? 
               PlayerPhysics::EYE_HEIGHT_SNEAKING : 
               PlayerPhysics::EYE_HEIGHT_STANDING;
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