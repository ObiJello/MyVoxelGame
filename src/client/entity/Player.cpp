// File: src/client/entity/Player.cpp
#include "Player.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include <glm/gtc/constants.hpp>

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

    void ClientPlayer::UpdatePhysics(float deltaTime, World* world) {
        // Update physics state based on input
        physics.isSneaking = sneakPressed;
        physics.isSprinting = sprintPressed && !sneakPressed;

        // Create physics context with world block access
        PhysicsContext context;
        context.blockAccess = world; // World implements IBlockAccess

        // Apply physics simulation with context
        UpdatePlayerPhysics(physics, movementInput, jumpPressed, sneakPressed, deltaTime, context);

        // Update predicted position from physics
        predictedPos = glm::dvec3(physics.position);

        // Reset single-frame inputs
        jumpPressed = false;

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
        if (pressed && !jumpPressed) {
            jumpPressed = true; // Only register the press edge
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