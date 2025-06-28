// File: src/game/PlayerController.hpp
#pragma once

#include "Inventory.hpp"
#include "RayCast.hpp"
#include "../render/Camera.hpp"
#include <optional>
#include <chrono>

namespace Game {

    class PlayerController {
    public:
        // Configuration
        static constexpr float INTERACTION_RANGE = 5.0f;
        static constexpr float BREAK_TIME = 0.25f; // Time to break a block in seconds
        static constexpr float PLACE_COOLDOWN = 0.1f; // Cooldown between block placements

        PlayerController();

        // Update the player controller (call once per frame)
        void Update(float deltaTime, const Render::Camera& camera);

        // Get the player's inventory
        Inventory& GetInventory() { return inventory; }
        const Inventory& GetInventory() const { return inventory; }

        // Get current raycast hit (if any)
        const std::optional<RaycastHit>& GetCurrentHit() const { return currentHit; }

        // Check if currently breaking a block
        bool IsBreaking() const { return isBreaking; }

        // Get breaking progress (0.0 to 1.0)
        float GetBreakProgress() const { return breakProgress; }

        // Input handlers (to be called from main loop based on input)
        void OnBreakPressed();
        void OnBreakReleased();
        void OnPlacePressed();
        void OnPlaceReleased();

        // Inventory slot selection
        void SelectSlot(int slot);
        void SelectNextSlot();
        void SelectPreviousSlot();

        // Statistics
        struct Stats {
            int blocksPlaced = 0;
            int blocksBroken = 0;
            int lastPlacedBlockId = -1;
            int lastBrokenBlockId = -1;
        };

        const Stats& GetStats() const { return stats; }

    private:
        Inventory inventory;
        std::optional<RaycastHit> currentHit;

        // Breaking state
        bool isBreaking;
        bool breakButtonHeld;
        float breakProgress;
        glm::ivec3 breakingBlockPos;

        // Placing state
        bool placeButtonHeld;
        float placeCooldownTimer;

        // Statistics
        Stats stats;

        // Helper methods
        void UpdateRaycast(const Render::Camera& camera);
        void UpdateBreaking(float deltaTime);
        void TryPlaceBlock();
        void FinishBreaking();
        bool CanPlaceBlockAt(const glm::ivec3& pos);

        // Get the block type that should be used for breaking effects
        BlockID GetBreakingBlockType(const glm::ivec3& pos);
    };

} // namespace Game