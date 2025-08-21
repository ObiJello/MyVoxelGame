// File: src/server/player/ServerPlayer.hpp
#pragma once

#include <glm/glm.hpp>
#include <string>
#include <memory>
#include <chrono>
#include <array>
#include "common/world/block/Blocks.hpp"
#include "common/world/math/WorldMath.hpp"

namespace Game {
    class World;
}

namespace Server {

    // Game modes matching Minecraft
    enum class GameMode {
        SURVIVAL = 0,
        CREATIVE = 1,
        ADVENTURE = 2,
        SPECTATOR = 3
    };

    // Damage sources for future implementation
    enum class DamageSource {
        GENERIC,
        FALL,
        FIRE,
        DROWNING,
        STARVATION,
        VOID,
        EXPLOSION,
        ENTITY_ATTACK,
        MAGIC
    };

    // Status effect placeholder for future implementation
    struct StatusEffect {
        int effectId;
        int duration;
        int amplifier;
    };

    // Server-side player entity representing authoritative gameplay state
    // This class owns all gameplay logic and state for a player
    class ServerPlayer {
    public:
        ServerPlayer(uint32_t playerId, const std::string& name);
        ~ServerPlayer();

        // === LIFECYCLE ===
        
        // Update player state for one server tick
        void tick(Game::World* world, int currentTick);
        
        // Respawn player at given position
        void respawn(const glm::vec3& spawnPos);
        
        // === MOVEMENT & PHYSICS ===
        
        // Apply movement intent from client input
        void applyMovementIntent(const glm::vec3& intent);
        
        // Teleport to position
        void teleport(const glm::dvec3& pos);
        
        // Set rotation (yaw, pitch)
        void setRotation(float yaw, float pitch);
        
        // Update position from client packet (with validation)
        void setPosition(const glm::dvec3& pos);
        
        // === BLOCK INTERACTIONS ===
        
        // Start breaking a block
        void startDestroyBlock(const glm::ivec3& pos, int face);
        
        // Stop breaking current block
        void stopDestroyBlock();
        
        // Continue breaking (update progress)
        void continueDestroyBlock(const glm::ivec3& pos);
        
        // Check if player can place block at position
        bool canPlaceAt(const glm::ivec3& pos, Game::BlockID block) const;
        
        // Try to place a block
        bool tryPlaceBlock(const glm::ivec3& pos, Game::BlockID block, int face);
        
        // === INVENTORY ===
        
        // Select hotbar slot
        void selectHotbarSlot(int slot);
        
        // Get currently held block/item
        Game::BlockID getHeldBlock() const;
        
        // Set block in hotbar slot
        void setHotbarBlock(int slot, Game::BlockID block);
        
        // === DAMAGE & EFFECTS ===
        
        // Apply damage to player
        void damage(float amount, DamageSource source);
        
        // Heal player
        void heal(float amount);
        
        // Add status effect
        void addEffect(const StatusEffect& effect);
        
        // Remove status effect
        void removeEffect(int effectId);
        
        // === ABILITIES ===
        
        // Set game mode
        void setGameMode(GameMode mode);
        
        // Set flying state
        void setFlying(bool flying);
        
        // Check if player can reach position
        bool canReach(const glm::vec3& pos) const;
        
        // === GETTERS ===
        
        uint32_t getPlayerId() const { return m_playerId; }
        const std::string& getName() const { return m_name; }
        
        const glm::dvec3& getPosition() const { return m_position; }
        float getYaw() const { return m_rotation.x; }
        float getPitch() const { return m_rotation.y; }
        const glm::vec2& getRotation() const { return m_rotation; }
        
        int getDimensionId() const { return m_dimensionId; }
        void setDimensionId(int id) { m_dimensionId = id; }
        
        float getHealth() const { return m_health; }
        int getFood() const { return m_food; }
        
        GameMode getGameMode() const { return m_gameMode; }
        bool isFlying() const { return m_flying; }
        bool canFly() const { return m_canFly; }
        
        bool isOnGround() const { return m_onGround; }
        void setOnGround(bool onGround) { m_onGround = onGround; }
        
        bool IsSneaking() const { return m_sneaking; }
        
        Game::Math::ChunkPos getChunkPosition() const {
            return Game::Math::ChunkPos(
                static_cast<int>(std::floor(m_position.x / 16.0)),
                static_cast<int>(std::floor(m_position.z / 16.0))
            );
        }
        
        // === STATISTICS ===
        
        std::chrono::steady_clock::time_point getLastUpdateTime() const { return m_lastUpdateTime; }
        void updateLastUpdateTime() { m_lastUpdateTime = std::chrono::steady_clock::now(); }

    private:
        // === IDENTITY ===
        uint32_t m_playerId;
        std::string m_name;
        // TODO: UUID m_uuid;
        // TODO: ProfileProperties m_profile; // skin data
        // TODO: PermissionLevel m_permissions;
        
        // === TRANSFORM & PHYSICS ===
        glm::dvec3 m_position{0.0, 67.0, 0.0};
        glm::vec2 m_rotation{0.0f, 0.0f}; // yaw, pitch
        glm::vec3 m_velocity{0.0f};
        bool m_onGround = true;
        bool m_sneaking = false;
        // TODO: AABB m_boundingBox;
        int m_dimensionId = 0;
        // TODO: glm::vec3 m_respawnPoint;
        
        // === ATTRIBUTES & STATUS ===
        float m_health = 20.0f;
        int m_food = 20;
        // TODO: float m_exhaustion = 0.0f;
        // TODO: int m_experienceLevel = 0;
        // TODO: float m_experienceProgress = 0.0f;
        // TODO: std::vector<StatusEffect> m_effects;
        float m_stepHeight = 0.6f;
        float m_fallDistance = 0.0f;
        
        // === ABILITIES & MODE ===
        GameMode m_gameMode = GameMode::SURVIVAL;
        bool m_canFly = false;
        bool m_flying = false;
        bool m_instabuild = false; // creative instant break
        float m_reachDistance = 5.0f;
        
        // === INVENTORY ===
        // TODO: PlayerInventory m_inventory;
        int m_selectedHotbarSlot = 0;
        // Basic hotbar inventory (9 slots)
        std::array<Game::BlockID, 9> m_hotbarBlocks = {
            Game::BlockID::Stone,
            Game::BlockID::Dirt,
            Game::BlockID::Grass,
            Game::BlockID::Cobblestone,
            Game::BlockID::OakPlanks,
            Game::BlockID::OakLog,
            Game::BlockID::Glass,
            Game::BlockID::Sand,
            Game::BlockID::Air
        };
        // TODO: ItemStack m_mainHand;
        // TODO: ItemStack m_offHand;
        // TODO: std::array<ItemStack, 4> m_armor;
        // TODO: Container* m_openContainer = nullptr;
        
        // === GAMEPLAY TIMERS ===
        int m_invulnerabilityTicks = 0;
        // TODO: int m_portalCooldown = 0;
        // TODO: int m_attackCooldown = 0;
        // TODO: int m_useCooldown = 0;
        // TODO: bool m_sleeping = false;
        
        // === MINING STATE ===
        bool m_isBreaking = false;
        glm::ivec3 m_breakingPos{0};
        float m_breakProgress = 0.0f;
        int m_breakStartTick = 0;
        
        // === TIMING ===
        std::chrono::steady_clock::time_point m_lastUpdateTime;
        
        // === INTERNAL METHODS ===
        
        // Update position with physics
        void updatePosition(Game::World* world);
        
        // Calculate break time for block
        float calculateBreakTime(Game::BlockID block) const;
        
        // Check collision at position
        bool checkCollision(Game::World* world, const glm::dvec3& pos) const;
    };

} // namespace Server