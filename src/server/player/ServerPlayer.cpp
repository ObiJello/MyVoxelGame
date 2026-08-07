// File: src/server/player/ServerPlayer.cpp
#include "ServerPlayer.hpp"
#include "common/entity/ConsumableBehavior.hpp"
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

        // m_inventoryMenu is built over m_inventory by its member initialiser;
        // only the game-mode-dependent flag needs setting here.
        m_inventoryMenu.creative = (m_gameMode == GameMode::CREATIVE);

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

        // Void damage — MC Entity.checkBelowWorld: 64 blocks below the world
        // floor (minY -64 → threshold -128) deals 4/hit until death. The
        // invulnerability window rate-limits it; creative/spectator are
        // immune inside damage().
        if (!m_isDead && m_position.y < -128.0) {
            damage(4.0f, DamageSource::VOID_DAMAGE);
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
        
        // Hunger / saturation / regen / starvation — MC FoodData.tick
        // (FoodData.java:32-73). Survival only: creative doesn't drain or
        // starve (MC gates via Player.tick's abilities checks — exhaustion
        // sources never fire in creative and food is hidden).
        if (m_gameMode == GameMode::SURVIVAL || m_gameMode == GameMode::ADVENTURE) {
            m_foodData.tick(*this);
        }

        // TODO: Handle portal cooldown
        // if (m_portalCooldown > 0) {
        //     m_portalCooldown--;
        // }
        
        // Item-use countdown (eating, blocking, …). Mirrors MC's
        // LivingEntity.baseTick → updatingUsingItem (LivingEntity.java:3254).
        updatingUsingItem();

        // Update position with physics (existing functionality)
        updatePosition(world);

        // Update mining progress
        if (m_isBreaking) {
            continueDestroyBlock(m_breakingPos);
        }
    }

    // === ITEM USE LIFECYCLE — mirrors LivingEntity.java:3246-3449 ===

    int ServerPlayer::handSlotIndex(uint32_t hand) const {
        return hand == 0
            ? Game::Inventory::HotbarToIndex(m_inventory.GetSelectedSlot())
            : Game::Inventory::OFFHAND_BEGIN;
    }

    Game::ItemStack& ServerPlayer::getItemInHand(uint32_t hand) {
        return m_inventory.MutableSlot(handSlotIndex(hand));
    }

    const Game::ItemStack& ServerPlayer::getItemInHand(uint32_t hand) const {
        return m_inventory.GetSlot(handSlotIndex(hand));
    }

    void ServerPlayer::setItemInHand(uint32_t hand, const Game::ItemStack& stack) {
        const int slot = handSlotIndex(hand);
        m_inventory.SetSlotFull(slot, stack);
        markSlotDirty(slot);
    }

    // Mirrors LivingEntity.startUsingItem (LivingEntity.java:3325-3340).
    void ServerPlayer::startUsingItem(uint32_t hand) {
        const Game::ItemStack& stack = getItemInHand(hand);
        if (!stack.IsEmpty() && !m_isUsingItem) {
            m_useItem          = stack;                          // :3328
            m_useItemRemaining = Game::GetUseDuration(stack);    // :3329
            m_isUsingItem      = true;                           // :3331 (flag bit 1)
            m_usedItemHand     = hand;                           // :3332 (flag bit 2)
            // :3333 causeUseVibration → game-event system TODO (log-stub level)
            // :3334-3336 KINETIC_WEAPON bookkeeping omitted — no combat.
        }
    }

    // Mirrors LivingEntity.updatingUsingItem (LivingEntity.java:3254-3264).
    void ServerPlayer::updatingUsingItem() {
        if (!m_isUsingItem) return;
        Game::ItemStack& inHand = getItemInHand(m_usedItemHand);
        // MC: `ItemStack.isSameItem(getItemInHand(hand), useItem)` — compare
        // item identity only; count changes (stacking) don't cancel the use.
        if (inHand.itemId == m_useItem.itemId && !inHand.IsEmpty()) {
            m_useItem = inHand;              // :3257 — refresh to the live stack
            updateUsingItem();               // :3258
        } else {
            stopUsingItem();                 // :3260
        }
    }

    // Mirrors LivingEntity.updateUsingItem (LivingEntity.java:3296-3302).
    void ServerPlayer::updateUsingItem() {
        // :3297 useItem.onUseTick → ItemStack.onUseTick (ItemStack.java:1060-1064)
        // — the periodic consume-phase eat sound/particle stub.
        Game::ConsumableBehavior::OnUseTick(*this, m_useItem, m_useItemRemaining);
        // :3298 — `--useItemRemaining == 0 && !useOnRelease → completeUsingItem`
        // (useOnRelease is crossbow-only; we have no item that sets it).
        if (--m_useItemRemaining == 0) {
            completeUsingItem();
        }
    }

    // Mirrors LivingEntity.completeUsingItem (LivingEntity.java:3388-3405).
    void ServerPlayer::completeUsingItem() {
        const uint32_t hand = m_usedItemHand;
        Game::ItemStack& inHand = getItemInHand(hand);
        if (inHand.itemId != m_useItem.itemId) {   // :3391 — hand changed under us
            releaseUsingItem();
            return;
        }
        if (!m_useItem.IsEmpty() && m_isUsingItem) {   // :3394
            Game::ItemStack result = finishUsingItem(inHand);   // :3395
            // Always write the (possibly mutated/replaced) stack back through
            // setItemInHand so the slot is marked dirty and broadcast. MC only
            // assigns when the reference changed (:3396-3398); with our
            // value-semantics stacks the write-through is how mutation lands.
            setItemInHand(hand, result);
            stopUsingItem();   // :3400
        }
    }

    // Mirrors ItemStack.finishUsingItem → Item.finishUsingItem
    // (Item.java:221-224) + applyAfterUseComponentSideEffects (USE_REMAINDER,
    // ItemStack.java:332-348). Delegated to ConsumableBehavior::FinishUsing.
    Game::ItemStack ServerPlayer::finishUsingItem(Game::ItemStack& stack) {
        return Game::ConsumableBehavior::FinishUsing(*this, stack);
    }

    // Mirrors LivingEntity.releaseUsingItem (LivingEntity.java:3426-3437).
    void ServerPlayer::releaseUsingItem() {
        Game::ItemStack& inHand = getItemInHand(m_usedItemHand);
        if (!m_useItem.IsEmpty() && inHand.itemId == m_useItem.itemId) {   // :3428
            m_useItem = inHand;   // :3429
            // :3430 useItem.releaseUsing(level, this, remaining) — per-item
            // release hook (Bow fires here). Default is a no-op
            // (Item.java:324-326); no item overrides it yet.
            // :3431-3433 useOnRelease (crossbow) omitted — no such item.
        }
        stopUsingItem();   // :3436
    }

    // Mirrors Player.isBlocking → getItemBlockingWith: the use must have
    // outlasted the item's blockDelayTicks (shield: 0.25 s = 5 ticks).
    bool ServerPlayer::isBlocking() const {
        if (!m_isUsingItem) return false;
        auto blocks = m_useItem.get(Game::DataComponents::BLOCKS_ATTACKS);
        if (!blocks) return false;
        return getTicksUsingItem() >= blocks->blockDelayTicks();
    }

    // Mirrors LivingEntity.stopUsingItem (LivingEntity.java:3439-3449).
    void ServerPlayer::stopUsingItem() {
        m_isUsingItem      = false;
        m_usedItemHand     = 0;
        m_useItem          = Game::ItemStack{};
        m_useItemRemaining = 0;
    }

    void ServerPlayer::respawn(const glm::vec3& spawnPos) {
        Log::Info("ServerPlayer: Respawning player %u at (%.1f, %.1f, %.1f)",
                 m_playerId, spawnPos.x, spawnPos.y, spawnPos.z);
        
        m_position = glm::dvec3(spawnPos);
        m_velocity = glm::vec3(0.0f);
        m_health = 20.0f;
        m_isDead = false;
        m_foodData.setFoodLevel(20);
        m_foodData.setSaturation(5.0f);
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
        // Open a brief grace window so the next few client-predicted
        // move packets (which may already be in flight at the new
        // position) don't trip the anti-cheat distance gate.
        m_teleportGraceUntil = std::chrono::steady_clock::now() +
                               std::chrono::seconds(2);
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
        
        // Check max distance from last position (anti-cheat).
        // Threshold must exceed the max portal-pair distance (~256 m
        // per the portal-gun reach cap) — otherwise the client's
        // predicted-teleport moves get rejected here, m_position never
        // advances to the destination, PortalRegistry::Tick never sees
        // the eye cross, the server-side teleport() never fires, and
        // the player gets wedged ("can't move past 100 blocks", chunks
        // around the destination never load). 600 leaves headroom for
        // long shots without disabling the check entirely.
        double distance = glm::length(pos - m_position);
        const bool inTeleportGrace =
            std::chrono::steady_clock::now() < m_teleportGraceUntil;
        if (distance > 600.0 && !inTeleportGrace &&
            m_gameMode != GameMode::CREATIVE &&
            m_gameMode != GameMode::SPECTATOR) {
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
        // Already dead — nothing left to kill (the death screen is up and
        // the body is frozen until PERFORM_RESPAWN).
        if (m_isDead) {
            return;
        }

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
            // Player died. Inventory is KEPT (no dropped-item-entity system —
            // deliberate deviation from MC's default). The health=0 in the
            // next SetHealthS2C push is the client's death signal (same as
            // MC), which opens the DeathScreen; PERFORM_RESPAWN revives via
            // respawn().
            m_isDead = true;
            m_isBreaking = false;
            stopUsingItem();
            Log::Info("ServerPlayer: Player %u died (source %d)",
                      m_playerId, static_cast<int>(source));
        }
    }

    void ServerPlayer::heal(float amount) {
        // Dead players don't regenerate — MC LivingEntity.heal is a no-op
        // when dead; without this, FoodData regen could quietly "revive" a
        // corpse waiting on the death screen.
        if (m_isDead) return;
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
        // Keeps the creative-only click paths (CLONE, creative grid, destroy
        // slot) in step with the gamemode.
        m_inventoryMenu.creative = (mode == GameMode::CREATIVE);

        // Mirrors MC GameType.updatePlayerAbilities: creative grants
        // mayfly/instabuild but does NOT force flying (you keep walking
        // until you double-tap space); only spectator forces flying.
        // Invulnerability is derived from the mode inside damage() — no
        // timer hack needed here.
        switch (mode) {
            case GameMode::CREATIVE:
                m_canFly = true;
                m_instabuild = true;
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