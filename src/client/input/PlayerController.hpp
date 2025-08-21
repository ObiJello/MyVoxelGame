// File: src/client/input/PlayerController.hpp
#pragma once

#include "../entity/Player.hpp"
#include "common/physics/RayCast.hpp"
#include <chrono>
#include <glm/glm.hpp>

// Forward declarations
namespace Client {
    class NetworkClient;
}

namespace Game {

    // Forward declaration
    class World;

    // Client-side player controller that handles interaction and (future) networking
    class ClientPlayerController {
    public:
        // Configuration
        static constexpr float INTERACTION_RANGE = 5.0f;
        static constexpr float BREAK_TIME = 0.25f; // Time to break a block in seconds
        static constexpr float PLACE_COOLDOWN = 0.1f; // Cooldown between block placements
        static constexpr float RIGHT_CLICK_DELAY = 0.2f; // 200ms delay between right clicks (Minecraft-style)

        ClientPlayerController();

        // Set references (must be called after creation)
        void SetPlayer(ClientPlayer* player);
        void SetWorld(World* world);
        void SetNetworkClient(Client::NetworkClient* networkClient);

        // Main update tick (call once per frame)
        void Tick(float deltaTime);

        // Input handlers (to be called from main loop based on input)
        void OnLMB(bool pressed);  // Left mouse button (break)
        void OnRMB(bool pressed);  // Right mouse button (place)
        
        // Hotbar selection
        void OnHotbarChanged(int slot);
        
        // Player commands
        void OnRespawnRequest();  // TODO: Implement for multiplayer
        
        // Check if currently breaking a block
        bool IsBreaking() const { return isBreaking; }

        // Get breaking progress (0.0 to 1.0)
        float GetBreakProgress() const { return breakProgress; }

        // Get player reference (for compatibility during refactor)
        ClientPlayer* GetPlayer() { return player; }
        const ClientPlayer* GetPlayer() const { return player; }

    private:
        // References
        ClientPlayer* player;
        World* world;
        Client::NetworkClient* networkClient;  // Network client for sending packets

        // Mining state
        bool isBreaking;
        bool breakButtonHeld;
        float breakProgress;
        glm::ivec3 breakingBlockPos;
        float mineSpeed = 1.0f;  // TODO: Calculate from tool/effects

        // Placing state
        bool placeButtonHeld;
        float placeCooldownTimer;
        float rightClickDelayTimer;  // Timer to prevent RMB spam (Minecraft-style)

        // Network state (placeholders for future implementation)
        std::chrono::steady_clock::time_point lastMoveSend;
        int moveSeq = 0;         // Movement sequence number
        int interactSeq = 0;     // Interaction sequence number
        bool sentPlayerLoaded = false;  // Track if we've sent initial spawn

        // Internal methods
        void SendMovementIfDue();  // TODO: Implement for networking
        void StartDig(const glm::ivec3& pos, int face);
        void AbortDig();
        void FinishDig();
        void SendUseItemOn(const RaycastHit& hit, int hand);  // TODO: Implement for networking
        void SendUseItem(int hand);  // TODO: Implement for networking

        // Helper methods (existing functionality)
        void UpdateBreaking(float deltaTime);
        void TryPlaceBlock();
        void FinishBreaking();  // Local block breaking implementation
        bool CanPlaceBlockAt(const glm::ivec3& pos);
        void MarkSurroundingSectionsForRemesh(const glm::ivec3& worldPos);
        BlockID GetBreakingBlockType(const glm::ivec3& pos);
    };

    // Typedef for compatibility during transition
    using PlayerController = ClientPlayerController;

} // namespace Game