// File: src/server/player/ServerPlayer.cpp
#include "ServerPlayer.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <cmath>

namespace Server {

    ServerPlayer::ServerPlayer(uint32_t playerId, const std::string& name)
        : m_playerId(playerId)
        , m_name(name) {
        m_lastUpdateTime = std::chrono::steady_clock::now();

        // Default starter inventory (replaces the old m_hotbarBlocks defaults).
        // Slot 0 (selected) stays empty; the rest mirror the old hardcoded set.
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(1), Game::BlockID::Dirt,      64);
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(2), Game::BlockID::Grass,     64);
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(3), Game::BlockID::Lava,      64);
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(4), Game::BlockID::Glass,     64);
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(5), Game::BlockID::Sand,      64);
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(6), Game::BlockID::OakLeaves, 64);
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(7), Game::BlockID::Water,     64);
        m_inventory.SetSlot(Game::Inventory::HotbarToIndex(8), Game::BlockID::Bedrock,   64);

        Log::Info("ServerPlayer: Created player %u '%s' at (%.1f, %.1f, %.1f)",
                 m_playerId, m_name.c_str(), m_position.x, m_position.y, m_position.z);
    }

    ServerPlayer::~ServerPlayer() {
        Log::Info("ServerPlayer: Destroyed player %u '%s'", m_playerId, m_name.c_str());
    }

    // === LIFECYCLE ===

    void ServerPlayer::tick(Game::World* world, int currentTick) {
        // TODO: Update invulnerability timer
        if (m_invulnerabilityTicks > 0) {
            m_invulnerabilityTicks--;
        }
        
        // TODO: Process status effects
        // for (auto& effect : m_effects) {
        //     effect.duration--;
        //     if (effect.duration <= 0) {
        //         removeEffect(effect.effectId);
        //     }
        // }
        
        // TODO: Handle drowning/suffocation
        // if (isUnderwater()) {
        //     m_airTicks--;
        //     if (m_airTicks <= 0) {
        //         damage(1.0f, DamageSource::DROWNING);
        //     }
        // }
        
        // TODO: Update fall distance
        // if (!m_onGround && m_velocity.y < 0) {
        //     m_fallDistance += -m_velocity.y;
        // } else if (m_onGround && m_fallDistance > 3.0f) {
        //     damage(m_fallDistance - 3.0f, DamageSource::FALL);
        //     m_fallDistance = 0.0f;
        // }
        
        // TODO: Process food/hunger
        // if (m_gameMode == GameMode::SURVIVAL) {
        //     m_exhaustion += 0.01f; // Base exhaustion
        //     if (m_exhaustion >= 4.0f) {
        //         m_food = std::max(0, m_food - 1);
        //         m_exhaustion = 0.0f;
        //     }
        // }
        
        // TODO: Handle portal cooldown
        // if (m_portalCooldown > 0) {
        //     m_portalCooldown--;
        // }
        
        // Update position with physics (existing functionality)
        updatePosition(world);
        
        // Update mining progress
        if (m_isBreaking) {
            continueDestroyBlock(m_breakingPos);
        }
    }

    void ServerPlayer::respawn(const glm::vec3& spawnPos) {
        Log::Info("ServerPlayer: Respawning player %u at (%.1f, %.1f, %.1f)",
                 m_playerId, spawnPos.x, spawnPos.y, spawnPos.z);
        
        m_position = glm::dvec3(spawnPos);
        m_velocity = glm::vec3(0.0f);
        m_health = 20.0f;
        m_food = 20;
        m_fallDistance = 0.0f;
        m_invulnerabilityTicks = 60; // 3 seconds of invulnerability
        
        // TODO: Clear status effects
        // m_effects.clear();
        
        // TODO: Reset inventory if keepInventory is false
        // if (!world->getGameRule("keepInventory")) {
        //     m_inventory.clear();
        // }
    }

    // === MOVEMENT & PHYSICS ===

    void ServerPlayer::applyMovementIntent(const glm::vec3& intent) {
        // TODO: Apply movement based on game mode and abilities
        if (m_flying) {
            // Flying movement
            m_velocity = intent * 0.5f; // Flying is faster
        } else {
            // Ground movement
            m_velocity.x = intent.x * 0.1f;
            m_velocity.z = intent.z * 0.1f;
            
            if (intent.y > 0 && m_onGround) {
                // Jump
                m_velocity.y = 0.42f; // Minecraft jump velocity
                m_onGround = false;
            }
        }
    }

    void ServerPlayer::teleport(const glm::dvec3& pos) {
        Log::Info("ServerPlayer: Teleporting player %u to (%.1f, %.1f, %.1f)",
                 m_playerId, pos.x, pos.y, pos.z);
        m_position = pos;
        m_velocity = glm::vec3(0.0f);
        m_fallDistance = 0.0f;
    }

    void ServerPlayer::setRotation(float yaw, float pitch) {
        m_rotation.x = yaw;
        m_rotation.y = std::clamp(pitch, -90.0f, 90.0f);
    }

    void ServerPlayer::setPosition(const glm::dvec3& pos) {
        // Basic validation
        if (std::isnan(pos.x) || std::isnan(pos.y) || std::isnan(pos.z)) {
            Log::Warning("ServerPlayer: Invalid position for player %u", m_playerId);
            return;
        }
        
        // Check max distance from last position (anti-cheat)
        double distance = glm::length(pos - m_position);
        if (distance > 100.0 && m_gameMode != GameMode::CREATIVE && m_gameMode != GameMode::SPECTATOR) {
            Log::Warning("ServerPlayer: Player %u moved too fast (%.1f blocks)", m_playerId, distance);
            // TODO: Send position correction to client
            return;
        }
        
        m_position = pos;
        updateLastUpdateTime();
    }

    // === BLOCK INTERACTIONS ===

    void ServerPlayer::startDestroyBlock(const glm::ivec3& pos, int face) {
        // Check if player can reach
        glm::vec3 blockCenter = glm::vec3(pos) + glm::vec3(0.5f);
        if (!canReach(blockCenter)) {
            Log::Warning("ServerPlayer: Player %u cannot reach block at (%d,%d,%d)",
                        m_playerId, pos.x, pos.y, pos.z);
            return;
        }
        
        m_isBreaking = true;
        m_breakingPos = pos;
        m_breakProgress = 0.0f;
        m_breakStartTick = 0; // TODO: Get current server tick
        
        Log::Debug("ServerPlayer: Player %u started breaking block at (%d,%d,%d)",
                  m_playerId, pos.x, pos.y, pos.z);
    }

    void ServerPlayer::stopDestroyBlock() {
        if (m_isBreaking) {
            Log::Debug("ServerPlayer: Player %u stopped breaking block", m_playerId);
            m_isBreaking = false;
            m_breakProgress = 0.0f;
        }
    }

    void ServerPlayer::continueDestroyBlock(const glm::ivec3& pos) {
        if (!m_isBreaking || pos != m_breakingPos) {
            return;
        }
        
        // TODO: Get block type and calculate break time
        // Game::BlockID blockId = world->getBlock(pos);
        // float breakTime = calculateBreakTime(blockId);
        float breakTime = 1.0f; // Default 1 second
        
        // Update progress
        m_breakProgress += 1.0f / (breakTime * 20.0f); // 20 ticks per second
        
        if (m_breakProgress >= 1.0f) {
            // Block is broken
            Log::Info("ServerPlayer: Player %u broke block at (%d,%d,%d)",
                     m_playerId, pos.x, pos.y, pos.z);
            
            // TODO: Drop items
            // TODO: Give experience
            // TODO: Update statistics
            
            stopDestroyBlock();
        }
    }

    bool ServerPlayer::canPlaceAt(const glm::ivec3& pos, Game::BlockID block) const {
        // Check if player can reach
        glm::vec3 blockCenter = glm::vec3(pos) + glm::vec3(0.5f);
        if (!canReach(blockCenter)) {
            return false;
        }
        
        // TODO: Check if position is valid for placement
        // - Not inside player bounding box
        // - Not replacing bedrock in survival
        // - Not outside world bounds
        // - Has permission to build here
        
        return true;
    }

    bool ServerPlayer::tryPlaceBlock(const glm::ivec3& pos, Game::BlockID block, int face) {
        if (!canPlaceAt(pos, block)) {
            return false;
        }
        
        // TODO: Check inventory for block
        // if (!m_inventory.hasItem(block)) {
        //     return false;
        // }
        
        // TODO: Remove block from inventory
        // m_inventory.removeItem(block, 1);
        
        Log::Info("ServerPlayer: Player %u placed block %d at (%d,%d,%d)",
                 m_playerId, static_cast<int>(block), pos.x, pos.y, pos.z);
        
        return true;
    }

    // === INVENTORY ===

    void ServerPlayer::selectHotbarSlot(int slot) {
        if (slot >= 0 && slot < Game::Inventory::HOTBAR_SIZE) {
            m_inventory.SetSelectedSlot(slot);
            Log::Debug("ServerPlayer: Player %u selected hotbar slot %d", m_playerId, slot);
        }
    }

    Game::BlockID ServerPlayer::getHeldBlock() const {
        return m_inventory.GetSelectedBlock();
    }

    void ServerPlayer::setHotbarBlock(int slot, Game::BlockID block) {
        if (slot >= 0 && slot < Game::Inventory::HOTBAR_SIZE) {
            // Default count of 64 keeps parity with the legacy setHotbarBlock(slot, block) callers.
            int count = (block == Game::BlockID::Air) ? 0 : 64;
            m_inventory.SetSlot(Game::Inventory::HotbarToIndex(slot), block, count);
            Log::Debug("ServerPlayer: Set hotbar slot %d to block %d", slot, static_cast<int>(block));
        }
    }

    // === DAMAGE & EFFECTS ===

    void ServerPlayer::damage(float amount, DamageSource source) {
        // TODO: Implement damage calculation
        
        // Check for invulnerability
        if (m_invulnerabilityTicks > 0) {
            return;
        }
        
        // Check game mode
        if (m_gameMode == GameMode::CREATIVE || m_gameMode == GameMode::SPECTATOR) {
            return;
        }
        
        // TODO: Apply armor reduction
        // amount = m_armor.reduceDamage(amount, source);
        
        // TODO: Apply resistance effects
        // for (const auto& effect : m_effects) {
        //     if (effect.effectId == RESISTANCE) {
        //         amount *= (1.0f - 0.2f * effect.amplifier);
        //     }
        // }
        
        // Apply damage
        m_health = std::max(0.0f, m_health - amount);
        m_invulnerabilityTicks = 10; // 0.5 seconds
        
        Log::Info("ServerPlayer: Player %u took %.1f damage from %d (health: %.1f)",
                 m_playerId, amount, static_cast<int>(source), m_health);
        
        // TODO: Send damage animation packet
        // TODO: Play hurt sound
        
        if (m_health <= 0.0f) {
            // Player died
            Log::Info("ServerPlayer: Player %u died", m_playerId);
            // TODO: Drop inventory
            // TODO: Send death message
            // TODO: Trigger respawn
        }
    }

    void ServerPlayer::heal(float amount) {
        // TODO: Implement healing
        if (m_health < 20.0f) {
            m_health = std::min(20.0f, m_health + amount);
            Log::Debug("ServerPlayer: Player %u healed %.1f (health: %.1f)",
                      m_playerId, amount, m_health);
        }
    }

    void ServerPlayer::addEffect(const StatusEffect& effect) {
        // TODO: Implement status effects
        // m_effects.push_back(effect);
        Log::Debug("ServerPlayer: Added effect %d to player %u", effect.effectId, m_playerId);
    }

    void ServerPlayer::removeEffect(int effectId) {
        // TODO: Implement status effect removal
        // m_effects.erase(std::remove_if(m_effects.begin(), m_effects.end(),
        //     [effectId](const StatusEffect& e) { return e.effectId == effectId; }),
        //     m_effects.end());
        Log::Debug("ServerPlayer: Removed effect %d from player %u", effectId, m_playerId);
    }

    // === ABILITIES ===

    void ServerPlayer::setGameMode(GameMode mode) {
        m_gameMode = mode;
        
        // Update abilities based on game mode
        switch (mode) {
            case GameMode::CREATIVE:
                m_canFly = true;
                m_instabuild = true;
                m_invulnerabilityTicks = -1; // Always invulnerable
                break;
            case GameMode::SPECTATOR:
                m_canFly = true;
                m_flying = true;
                m_instabuild = false;
                break;
            case GameMode::SURVIVAL:
            case GameMode::ADVENTURE:
                m_canFly = false;
                m_flying = false;
                m_instabuild = false;
                m_invulnerabilityTicks = 0;
                break;
        }
        
        Log::Info("ServerPlayer: Player %u game mode changed to %d", m_playerId, static_cast<int>(mode));
    }

    void ServerPlayer::setFlying(bool flying) {
        if (m_canFly) {
            m_flying = flying;
            if (!flying) {
                // TODO: Check if player will fall
            }
            Log::Debug("ServerPlayer: Player %u flying set to %s", m_playerId, flying ? "true" : "false");
        }
    }

    bool ServerPlayer::canReach(const glm::vec3& pos) const {
        // Calculate distance from eye position
        glm::vec3 eyePos = glm::vec3(m_position) + glm::vec3(0.0f, 1.62f, 0.0f); // Eye height
        float distance = glm::length(pos - eyePos);
        
        return distance <= m_reachDistance;
    }

    // === INTERNAL METHODS ===

    void ServerPlayer::updatePosition(Game::World* world) {
        if (!world) return;
        
        // Apply gravity if not flying
        if (!m_flying && !m_onGround) {
            m_velocity.y -= 0.08f; // Minecraft gravity
            m_velocity.y = std::max(-3.92f, m_velocity.y); // Terminal velocity
        }
        
        // Apply velocity
        glm::dvec3 newPos = m_position + glm::dvec3(m_velocity);
        
        // TODO: Check collision
        if (!checkCollision(world, newPos)) {
            m_position = newPos;
        } else {
            // Hit something, stop velocity in that direction
            m_velocity = glm::vec3(0.0f);
            if (newPos.y < m_position.y) {
                m_onGround = true;
            }
        }
        
        // Apply friction
        if (m_onGround) {
            m_velocity.x *= 0.6f;
            m_velocity.z *= 0.6f;
        } else {
            m_velocity.x *= 0.98f;
            m_velocity.z *= 0.98f;
        }
    }

    float ServerPlayer::calculateBreakTime(Game::BlockID block) const {
        // TODO: Implement proper break time calculation
        // Based on:
        // - Block hardness
        // - Tool type and material
        // - Efficiency enchantment
        // - Haste/Mining Fatigue effects
        // - Underwater penalty
        
        // Placeholder: all blocks take 1 second
        return 1.0f;
    }

    bool ServerPlayer::checkCollision(Game::World* world, const glm::dvec3& pos) const {
        // TODO: Implement proper AABB collision detection
        // For now, just check if the block at feet position is solid
        
        int blockX = static_cast<int>(std::floor(pos.x));
        int blockY = static_cast<int>(std::floor(pos.y));
        int blockZ = static_cast<int>(std::floor(pos.z));
        
        Game::BlockID block = world->GetBlock(blockX, blockY, blockZ);
        return block != Game::BlockID::Air;
    }

} // namespace Server