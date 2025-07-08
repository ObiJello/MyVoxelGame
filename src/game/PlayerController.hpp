// File: src/game/PlayerController.hpp (Updated with Physics)
#pragma once

#include "Inventory.hpp"
#include "../engine/physics/RayCast.hpp"
#include "../engine/physics/Physics.hpp"
#include "../render/gfx/Camera.hpp"
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
        void Update(float deltaTime, Render::Camera& camera);

        // Get the player's inventory
        Inventory& GetInventory() { return inventory; }
        const Inventory& GetInventory() const { return inventory; }

        // Get the player's physics state
        PlayerPhysics& GetPhysics() { return physics; }
        const PlayerPhysics& GetPhysics() const { return physics; }

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

        // Movement input handlers
        void SetMovementInput(const glm::vec3& movement);
        void SetJumpPressed(bool pressed);
        void SetSprintPressed(bool pressed);
        void SetSneakPressed(bool pressed);

        // Inventory slot selection
        void SelectSlot(int slot);
        void SelectNextSlot();
        void SelectPreviousSlot();

        // Debug/cheat functions
        void ToggleNoclip();
        void SetNoclip(bool enabled);

        // Statistics
        struct Stats {
            int blocksPlaced = 0;
            int blocksBroken = 0;
            int lastPlacedBlockId = -1;
            int lastBrokenBlockId = -1;
            float totalDistanceTraveled = 0.0f;
            float totalPlayTime = 0.0f;
        };

        const Stats& GetStats() const { return stats; }

    private:
        Inventory inventory;
        PlayerPhysics physics;  // Player physics state
        std::optional<RaycastHit> currentHit;

        // Movement input state
        glm::vec3 movementInput{0.0f};
        bool jumpPressed = false;
        bool sprintPressed = false;
        bool sneakPressed = false;

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
        glm::vec3 lastPosition{0.0f}; // For distance tracking

        // Helper methods
        void UpdatePhysics(float deltaTime);
        void UpdateCamera(Render::Camera& camera);
        void UpdateRaycast(const Render::Camera& camera);
        void UpdateBreaking(float deltaTime);
        void TryPlaceBlock();
        void FinishBreaking();
        bool CanPlaceBlockAt(const glm::ivec3& pos);
        BlockID GetBreakingBlockType(const glm::ivec3& pos);
    };

} // namespace Game