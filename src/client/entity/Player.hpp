// File: src/client/entity/Player.hpp
#pragma once

#include "common/entity/Inventory.hpp"
#include "common/physics/RayCast.hpp"
#include "common/physics/Physics.hpp"
#include "../renderer/core/Camera.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <chrono>

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
        
        // Visual smoothing (for interpolation)
        glm::dvec3 visualPos{0.0, 67.0, 0.0};     // Smoothed position for rendering
        float visualYaw = 0.0f;                   // Smoothed yaw
        float visualPitch = 0.0f;                 // Smoothed pitch
        
        // === Player Attributes ===
        
        // Status (placeholders for future server sync)
        int health = 20;           // TODO: Sync from server
        int food = 20;             // TODO: Sync from server
        int air = 300;             // TODO: Sync from server (ticks of air remaining)
        float stepHeight = 0.6f;   // How high the player can step up
        
        // === Inventory ===
        Inventory inventory;
        
        // === Raycast Cache ===
        std::optional<RaycastHit> lastBlockHit;  // Cached result from per-frame raycast
        // TODO: Add entity hit cache when entity system is implemented
        // std::optional<EntityHit> lastEntityHit;
        
        // === Input State ===
        glm::vec3 movementInput{0.0f};
        bool jumpPressed = false;
        bool sprintPressed = false;
        bool sneakPressed = false;
        
        // === Statistics ===
        PlayerStats stats;
        
        // === Public Methods ===
        
        // Initialize player state
        void Initialize();
        
        // Update physics simulation (accepts any IBlockAccess: World*, ClientBlockAccess*, etc.)
        void UpdatePhysics(float deltaTime, IBlockAccess* blockAccess);
        
        // Update raycast from camera position
        void UpdateRaycast(const Render::Camera& camera);
        
        // Update visual smoothing (lerp visual toward predicted)
        void UpdateVisual(float deltaTime);
        
        // Apply server position correction
        void ApplyServerCorrection(const glm::dvec3& pos, float newYaw, float newPitch);
        
        // Get current eye position (for camera)
        glm::vec3 GetEyePosition() const;
        
        // Get current eye height based on pose
        float GetEyeHeight() const;
        
        // Movement input setters
        void SetMovementInput(const glm::vec3& movement) { movementInput = movement; }
        void SetJumpPressed(bool pressed);
        void SetSprintPressed(bool pressed) { sprintPressed = pressed; }
        void SetSneakPressed(bool pressed) { sneakPressed = pressed; }
        
        // Noclip control
        void ToggleNoclip();
        void SetNoclip(bool enabled);
        
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
        glm::vec3 lastPosition{0.0f};
        
        // Smoothing parameters
        static constexpr float POSITION_SMOOTHING_FACTOR = 10.0f;  // How fast visual lerps to predicted
        static constexpr float ROTATION_SMOOTHING_FACTOR = 15.0f;  // How fast rotation lerps
    };

} // namespace Game