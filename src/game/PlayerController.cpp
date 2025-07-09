// File: src/game/PlayerController.cpp
#include "PlayerController.hpp"
#include "../engine/block/BlockRegistry.hpp"
#include "world/World.hpp"
#include "../core/Log.hpp"
#include <glm/glm.hpp>
#include <thread>
#include <chrono>

namespace Game {

    PlayerController::PlayerController()
        : isBreaking(false)
        , breakButtonHeld(false)
        , breakProgress(0.0f)
        , breakingBlockPos(0)
        , placeButtonHeld(false)
        , placeCooldownTimer(0.0f)
        , world(nullptr) // Will be set later
    {
        // Initialize inventory with default blocks
        inventory.InitializeDefaults();

        // Initialize physics at a safe spawn position
        physics.position = glm::vec3(0.0f, 80.0f, 0.0f);
        physics.velocity = glm::vec3(0.0f);
        physics.isOnGround = false;
        physics.isSneaking = false;
        physics.isSprinting = false;
        physics.noclip = false;

        lastPosition = physics.position;

        Log::Info("PlayerController initialized with physics system");
    }

    void PlayerController::SetWorld(World* worldPtr) {
        world = worldPtr;
        Log::Debug("PlayerController world reference set");
    }

    void PlayerController::Update(float deltaTime, Render::Camera& camera) {
        // Update physics state based on input
        physics.isSneaking = sneakPressed;
        physics.isSprinting = sprintPressed && !sneakPressed; // Can't sprint while sneaking

        // Update physics simulation
        UpdatePhysics(deltaTime);

        // Update camera position to follow player
        UpdateCamera(camera);

        // Update raycast from camera position
        UpdateRaycast(camera);

        // Update breaking progress
        UpdateBreaking(deltaTime);

        // Update place cooldown
        if (placeCooldownTimer > 0.0f) {
            placeCooldownTimer -= deltaTime;
        }

        // Try to place block if button is held and cooldown expired
        if (placeButtonHeld && placeCooldownTimer <= 0.0f) {
            TryPlaceBlock();
        }

        // Update statistics
        stats.totalPlayTime += deltaTime;

        // Calculate distance
        float distanceThisFrame = glm::length(physics.position - lastPosition);
        stats.totalDistanceTraveled += distanceThisFrame;
        lastPosition = physics.position;
    }

    void PlayerController::UpdatePhysics(float deltaTime) {
        // Apply physics simulation - FIXED: Now passes sneakPressed parameter
        UpdatePlayerPhysics(physics, movementInput, jumpPressed, sneakPressed, deltaTime);

        // Reset single-frame inputs
        jumpPressed = false;
    }

    void PlayerController::UpdateCamera(Render::Camera& camera) {
        // Set camera position to player's eye position
        camera.position = physics.GetEyePosition();

        // If noclip is enabled, allow free camera movement
        if (physics.noclip) {
            // In noclip mode, camera position might be set independently
            // Store camera position back to physics for consistency
            physics.position = camera.position - glm::vec3(0.0f, physics.EYE_HEIGHT_STANDING, 0.0f);
        }
    }

    void PlayerController::UpdateRaycast(const Render::Camera& camera) {
        // Calculate ray direction from camera
        glm::vec3 front;
        front.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        front.y = sin(glm::radians(camera.pitch));
        front.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        front = glm::normalize(front);

        // Cast ray from camera position (player's eyes)
        currentHit = Raycast::CastRay(camera.position, front, INTERACTION_RANGE);

        // If we're breaking a block but lost sight of it, cancel breaking
        if (isBreaking && (!currentHit.has_value() ||
            currentHit->blockPos != breakingBlockPos)) {
            isBreaking = false;
            breakProgress = 0.0f;
            Log::Debug("Breaking cancelled - block out of sight");
        }
    }

    void PlayerController::UpdateBreaking(float deltaTime) {
        if (!isBreaking || !breakButtonHeld) {
            return;
        }

        // Increase break progress
        breakProgress += deltaTime / BREAK_TIME;

        // Check if block is broken
        if (breakProgress >= 1.0f) {
            FinishBreaking();
        }
    }

    void PlayerController::SetMovementInput(const glm::vec3& movement) {
        movementInput = movement;
    }

    void PlayerController::SetJumpPressed(bool pressed) {
        if (pressed && !jumpPressed) {
            jumpPressed = true; // Only register the press edge
        }
    }

    void PlayerController::SetSprintPressed(bool pressed) {
        sprintPressed = pressed;
    }

    void PlayerController::SetSneakPressed(bool pressed) {
        sneakPressed = pressed;
    }

    void PlayerController::OnBreakPressed() {
        breakButtonHeld = true;

        if (currentHit.has_value() && !isBreaking) {
            isBreaking = true;
            breakProgress = 0.0f;
            breakingBlockPos = currentHit->blockPos;

            Log::Debug("Started breaking block at (%d, %d, %d)",
                      breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z);
        }
    }

    void PlayerController::OnBreakReleased() {
        breakButtonHeld = false;

        if (isBreaking) {
            isBreaking = false;
            breakProgress = 0.0f;
            Log::Debug("Breaking cancelled - button released");
        }
    }

    void PlayerController::OnPlacePressed() {
        placeButtonHeld = true;

        if (placeCooldownTimer <= 0.0f) {
            TryPlaceBlock();
        }
    }

    void PlayerController::OnPlaceReleased() {
        placeButtonHeld = false;
    }

    void PlayerController::TryPlaceBlock() {
        if (!world) {
            Log::Warning("Cannot place block - no world reference");
            return;
        }

        if (!currentHit.has_value()) {
            return;
        }

        BlockID selectedBlock = inventory.GetSelectedBlock();
        if (selectedBlock == BlockID::Air) {
            return;
        }

        const glm::ivec3& placePos = currentHit->adjacentPos;
        if (!CanPlaceBlockAt(placePos)) {
            return;
        }

        // Check if placing block would intersect with player
        AABB blockAABB(
            glm::vec3(placePos) + glm::vec3(0.5f),
            glm::vec3(1.0f)
        );

        if (physics.GetAABB().Intersects(blockAABB)) {
            Log::Debug("Cannot place block - would intersect with player");
            return;
        }

        if (!inventory.ConsumeSelectedBlock()) {
            Log::Debug("Cannot place block - none left in inventory");
            return;
        }

        bool placementSuccessful = false;
        try {
            placementSuccessful = world->SetBlock(placePos.x, placePos.y, placePos.z, selectedBlock);
        } catch (const std::exception& e) {
            Log::Error("Exception during block placement: %s", e.what());
            placementSuccessful = false;
        }

        if (placementSuccessful) {
            stats.blocksPlaced++;
            stats.lastPlacedBlockId = static_cast<int>(selectedBlock);
            placeCooldownTimer = PLACE_COOLDOWN;

            const Block& block = BlockRegistry::Get(selectedBlock);
            Log::Info("Placed %s at (%d, %d, %d)",
                     block.name.c_str(), placePos.x, placePos.y, placePos.z);
        } else {
            inventory.AddBlocks(selectedBlock, 1);
            Log::Warning("Failed to place block at (%d, %d, %d)",
                        placePos.x, placePos.y, placePos.z);
        }
    }

    void PlayerController::FinishBreaking() {
        if (!world) {
            Log::Warning("Cannot break block - no world reference");
            return;
        }

        if (!currentHit.has_value()) {
            return;
        }

        BlockID brokenBlock = BlockID::Air;
        try {
            brokenBlock = world->GetBlock(breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z);
        } catch (const std::exception& e) {
            Log::Error("Exception getting block for breaking: %s", e.what());
            isBreaking = false;
            breakProgress = 0.0f;
            return;
        }

        if (brokenBlock == BlockID::Bedrock) {
            Log::Debug("Cannot break bedrock");
            isBreaking = false;
            breakProgress = 0.0f;
            return;
        }

        if (brokenBlock == BlockID::Air) {
            Log::Debug("Cannot break air");
            isBreaking = false;
            breakProgress = 0.0f;
            return;
        }

        bool breakingSuccessful = false;
        try {
            breakingSuccessful = world->SetBlock(breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z, BlockID::Air);
        } catch (const std::exception& e) {
            Log::Error("Exception during block breaking: %s", e.what());
            breakingSuccessful = false;
        }

        if (breakingSuccessful) {
            int remaining = inventory.AddBlocks(brokenBlock, 1);

            if (remaining > 0) {
                Log::Warning("Inventory full - dropped %d %s",
                           remaining, BlockRegistry::Get(brokenBlock).name.c_str());
            }

            stats.blocksBroken++;
            stats.lastBrokenBlockId = static_cast<int>(brokenBlock);

            const Block& block = BlockRegistry::Get(brokenBlock);
            Log::Info("Broke %s at (%d, %d, %d)",
                     block.name.c_str(), breakingBlockPos.x,
                     breakingBlockPos.y, breakingBlockPos.z);
        } else {
            Log::Warning("Failed to break block at (%d, %d, %d)",
                        breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z);
        }

        isBreaking = false;
        breakProgress = 0.0f;
    }

    bool PlayerController::CanPlaceBlockAt(const glm::ivec3& pos) {
        if (!world) {
            return false;
        }

        if (!world->IsValidPosition(pos.x, pos.y, pos.z)) {
            return false;
        }

        BlockID existing = BlockID::Air;
        try {
            existing = world->GetBlock(pos.x, pos.y, pos.z);
        } catch (const std::exception& e) {
            Log::Error("Exception checking block at placement position: %s", e.what());
            return false;
        }

        if (existing != BlockID::Air) {
            return false;
        }

        return true;
    }

    void PlayerController::SelectSlot(int slot) {
        inventory.SetSelectedSlot(slot);
    }

    void PlayerController::SelectNextSlot() {
        inventory.SelectNextSlot();
    }

    void PlayerController::SelectPreviousSlot() {
        inventory.SelectPreviousSlot();
    }

    void PlayerController::ToggleNoclip() {
        physics.noclip = !physics.noclip;
        Log::Info("Noclip %s", physics.noclip ? "enabled" : "disabled");

        if (physics.noclip) {
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
        }
    }

    void PlayerController::SetNoclip(bool enabled) {
        physics.noclip = enabled;
        Log::Info("Noclip %s", physics.noclip ? "enabled" : "disabled");

        if (physics.noclip) {
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
        }
    }

    BlockID PlayerController::GetBreakingBlockType(const glm::ivec3& pos) {
        if (!world) {
            return BlockID::Air;
        }

        try {
            return world->GetBlock(pos.x, pos.y, pos.z);
        } catch (const std::exception& e) {
            Log::Error("Exception getting breaking block type: %s", e.what());
            return BlockID::Air;
        }
    }

} // namespace Game