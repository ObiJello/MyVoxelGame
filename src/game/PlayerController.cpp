// File: src/game/PlayerController.cpp
#include "PlayerController.hpp"
#include "WorldAccess.hpp"
#include "BlockRegistry.hpp"
#include "WorldManager.hpp"
#include "../core/Log.hpp"
#include <glm/glm.hpp>

namespace Game {

    PlayerController::PlayerController()
        : isBreaking(false)
        , breakButtonHeld(false)
        , breakProgress(0.0f)
        , breakingBlockPos(0)
        , placeButtonHeld(false)
        , placeCooldownTimer(0.0f)
    {
        // Initialize inventory with default blocks
        inventory.InitializeDefaults();

        // Register for chunk modification notifications
        WorldAccess::RegisterModificationCallback([](Math::ChunkPos pos) {
            // Request remesh for the modified chunk
            WorldManager::ForceRemeshChunk(pos);

            Log::Debug("Chunk (%d, %d) marked for remeshing due to block modification",
                      pos.x, pos.z);
        });
    }

    void PlayerController::Update(float deltaTime, const Render::Camera& camera) {
        // Update raycast
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
    }

    void PlayerController::UpdateRaycast(const Render::Camera& camera) {
        // Calculate ray direction from camera
        glm::vec3 front;
        front.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        front.y = sin(glm::radians(camera.pitch));
        front.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        front = glm::normalize(front);

        // Cast ray
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

    void PlayerController::OnBreakPressed() {
        breakButtonHeld = true;

        if (currentHit.has_value() && !isBreaking) {
            // Start breaking the block we're looking at
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
            // Cancel breaking
            isBreaking = false;
            breakProgress = 0.0f;
            Log::Debug("Breaking cancelled - button released");
        }
    }

    void PlayerController::OnPlacePressed() {
        placeButtonHeld = true;

        // Try to place immediately if cooldown allows
        if (placeCooldownTimer <= 0.0f) {
            TryPlaceBlock();
        }
    }

    void PlayerController::OnPlaceReleased() {
        placeButtonHeld = false;
    }

    void PlayerController::TryPlaceBlock() {
        // Check if we have a valid hit and a block to place
        if (!currentHit.has_value()) {
            return;
        }

        BlockID selectedBlock = inventory.GetSelectedBlock();
        if (selectedBlock == BlockID::Air) {
            return;
        }

        // Check if we can place at the adjacent position
        const glm::ivec3& placePos = currentHit->adjacentPos;
        if (!CanPlaceBlockAt(placePos)) {
            return;
        }

        // Try to consume a block from inventory
        if (!inventory.ConsumeSelectedBlock()) {
            Log::Debug("Cannot place block - none left in inventory");
            return;
        }

        // Place the block
        if (WorldAccess::SetBlock(placePos.x, placePos.y, placePos.z, selectedBlock)) {
            // Update statistics
            stats.blocksPlaced++;
            stats.lastPlacedBlockId = static_cast<int>(selectedBlock);

            // Reset cooldown
            placeCooldownTimer = PLACE_COOLDOWN;

            const Block& block = BlockRegistry::Get(selectedBlock);
            Log::Info("Placed %s at (%d, %d, %d)",
                     block.name.c_str(), placePos.x, placePos.y, placePos.z);
        } else {
            // Failed to place, give the block back
            inventory.AddBlocks(selectedBlock, 1);
            Log::Warning("Failed to place block at (%d, %d, %d)",
                        placePos.x, placePos.y, placePos.z);
        }
    }

    void PlayerController::FinishBreaking() {
        if (!currentHit.has_value()) {
            return;
        }

        // Get the block type before breaking
        BlockID brokenBlock = WorldAccess::GetBlock(
            breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z);

        // Don't break bedrock
        if (brokenBlock == BlockID::Bedrock) {
            Log::Debug("Cannot break bedrock");
            isBreaking = false;
            breakProgress = 0.0f;
            return;
        }

        // Remove the block
        if (WorldAccess::SetBlock(breakingBlockPos.x, breakingBlockPos.y,
                                 breakingBlockPos.z, BlockID::Air)) {
            // Add the broken block to inventory
            int remaining = inventory.AddBlocks(brokenBlock, 1);

            if (remaining > 0) {
                Log::Warning("Inventory full - dropped %d %s",
                           remaining, BlockRegistry::Get(brokenBlock).name.c_str());
            }

            // Update statistics
            stats.blocksBroken++;
            stats.lastBrokenBlockId = static_cast<int>(brokenBlock);

            const Block& block = BlockRegistry::Get(brokenBlock);
            Log::Info("Broke %s at (%d, %d, %d)",
                     block.name.c_str(), breakingBlockPos.x,
                     breakingBlockPos.y, breakingBlockPos.z);
        }

        // Reset breaking state
        isBreaking = false;
        breakProgress = 0.0f;
    }

    bool PlayerController::CanPlaceBlockAt(const glm::ivec3& pos) {
        // Check if position is valid
        if (!WorldAccess::IsValidPosition(pos.x, pos.y, pos.z)) {
            return false;
        }

        // Check if space is empty
        BlockID existing = WorldAccess::GetBlock(pos.x, pos.y, pos.z);
        if (existing != BlockID::Air) {
            return false;
        }

        // TODO: Add collision check with player position to prevent placing blocks inside player

        return true;
    }

    void PlayerController::SelectSlot(int slot) {
        inventory.SetSelectedSlot(slot);
        Log::Debug("Selected inventory slot %d", inventory.GetSelectedSlot());
    }

    void PlayerController::SelectNextSlot() {
        inventory.SelectNextSlot();
        Log::Debug("Selected inventory slot %d", inventory.GetSelectedSlot());
    }

    void PlayerController::SelectPreviousSlot() {
        inventory.SelectPreviousSlot();
        Log::Debug("Selected inventory slot %d", inventory.GetSelectedSlot());
    }

    BlockID PlayerController::GetBreakingBlockType(const glm::ivec3& pos) {
        return WorldAccess::GetBlock(pos.x, pos.y, pos.z);
    }

} // namespace Game