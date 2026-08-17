// File: src/server/player/ServerPlayer.hpp
#pragma once

#include <glm/glm.hpp>
#include <string>
#include <memory>
#include <chrono>
#include <array>
#include <vector>
#include <optional>
#include <cstdint>
#include "common/world/block/Blocks.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/entity/Inventory.hpp"
#include "common/inventory/InventoryMenu.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "FoodData.hpp"
#include "PlayerExperience.hpp"

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
        VOID_DAMAGE,
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
    class ServerPlayer : public Game::IUsePlayer {
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

        // Get block in specific hotbar slot (slot is 0..8). Returns Air for non-block items.
        Game::BlockID getHotbarBlock(int slot) const {
            return m_inventory.GetSlot(Game::Inventory::HotbarToIndex(slot)).AsBlockID();
        }

        // Set block in hotbar slot
        void setHotbarBlock(int slot, Game::BlockID block);

        // Direct access to backing inventory (server-authoritative)
        Game::Inventory&       getInventory()       { return m_inventory; }
        const Game::Inventory& getInventory() const { return m_inventory; }

        // Cursor item (carried while a menu is open). Lives on the menu so
        // Game::AbstractContainerMenu::DoClick drives it identically here
        // (authority) and on the client (prediction).
        // Routed through the CURRENT menu, not m_inventoryMenu: in MC the cursor
        // belongs to whichever menu is open and is handed over by
        // transferState when one replaces another — see openContainerMenu /
        // closeContainerMenu, which carry it across the swap.
        Game::InventorySlot&       getCarried()       { return m_containerMenu->getCarried(); }
        const Game::InventorySlot& getCarried() const { return m_containerMenu->getCarried(); }
        void setCarried(const Game::InventorySlot& s) { m_containerMenu->setCarried(s); }

        // The menu clicks are dispatched against. Mirrors MC ServerPlayer's
        // inventoryMenu (always present) / containerMenu (the one on top —
        // swapped when a crafting table is opened, restored on close).
        //
        // `creative` is refreshed on every access rather than once at
        // construction so a game-mode change can never leave it stale.
        Game::AbstractContainerMenu& container() {
            m_containerMenu->creative = (m_gameMode == GameMode::CREATIVE);
            return *m_containerMenu;
        }
        Game::InventoryMenu& inventoryMenu() { return m_inventoryMenu; }
        // True while a BLOCK container (not the player's own menu) is open.
        bool hasOpenContainerMenu() const { return m_openContainerMenu != nullptr; }
        Game::MenuType openMenuType() const { return m_openMenuType; }

        // Put `menu` on top (MC ServerPlayer.openMenu → containerMenu = ...).
        // The cursor and the container id ride across, and the id is bumped so
        // clicks still in flight for the previous menu are rejected.
        void openContainerMenu(std::unique_ptr<Game::AbstractContainerMenu> menu,
                               Game::MenuType type);
        // Back to the player's own menu (MC doCloseContainer). Returns the
        // outgoing menu's Removed() result so the caller can rebroadcast the
        // slots it handed back; empty when nothing but the inventory was open.
        Game::ContainerClickResult closeContainerMenu();

        // ── IUsePlayer: pending menu request ──────────────────────────────
        // A block asked to open its UI during use dispatch. Recorded rather
        // than acted on, because opening needs the network connection that
        // PlayerSession owns — it drains this the moment dispatch returns.
        struct PendingMenuOpen {
            Game::MenuType type;
            glm::ivec3     pos;
        };
        void OpenMenu(Game::MenuType type, const glm::ivec3& pos) override {
            m_pendingMenuOpen = PendingMenuOpen{type, pos};
        }
        std::optional<PendingMenuOpen> takePendingMenuOpen() {
            auto pending = m_pendingMenuOpen;
            m_pendingMenuOpen.reset();
            return pending;
        }

        // ── IUsePlayer: pending campfire food placement ───────────────────
        // Recorded for the same reason as the menu request above: reaching the
        // campfire's block entity needs the world, which PlayerSession has and
        // the use dispatch does not.
        struct PendingCampfireFood {
            glm::ivec3 pos;
            uint32_t   hand;
        };
        void PlaceCampfireFood(const glm::ivec3& pos, uint32_t hand) override {
            m_pendingCampfireFood = PendingCampfireFood{pos, hand};
        }
        std::optional<PendingCampfireFood> takePendingCampfireFood() {
            auto pending = m_pendingCampfireFood;
            m_pendingCampfireFood.reset();
            return pending;
        }

        // === HAND SLOTS ===
        // hand 0 = main hand (hotbar 36 + selected), hand 1 = offhand (slot 45).
        // Mirrors MC Player.getItemInHand / setItemInHand.
        Game::ItemStack&       getItemInHand(uint32_t hand) override;
        const Game::ItemStack& getItemInHand(uint32_t hand) const;
        // Writes the stack AND records the slot in m_dirtySlots so
        // PlayerSession broadcasts the delta after the tick.
        void setItemInHand(uint32_t hand, const Game::ItemStack& stack);
        // Unified inventory index for a hand (36+selected / 45).
        int  handSlotIndex(uint32_t hand) const override;

        // === ITEM USE STATE — mirrors LivingEntity.java:3246-3449 ===
        // The hold-to-use lifecycle: startUsingItem sets useItem + the
        // remaining-tick countdown; tick() → updatingUsingItem() counts down
        // and fires completeUsingItem (timer hit zero — eat finished) or the
        // caller fires releaseUsingItem (player let go early — RELEASE_USE_ITEM).
        bool     isUsingItem() const { return m_isUsingItem; }        // :3246-3248
        uint32_t getUsedItemHand() const { return m_usedItemHand; }   // :3250-3252
        int      getUseItemRemainingTicks() const { return m_useItemRemaining; } // :3414-3416
        // Elapsed ticks since use started (:3418-3420).
        int      getTicksUsingItem() const {
            return m_isUsingItem ? Game::GetUseDuration(m_useItem) - m_useItemRemaining : 0;
        }
        const Game::ItemStack& getUseItem() const { return m_useItem; } // :3410-3412

        void startUsingItem(uint32_t hand);   // LivingEntity.java:3325-3340
        void releaseUsingItem();              // :3426-3437
        void stopUsingItem();                 // :3439-3449
        void completeUsingItem();             // :3388-3405

        // Mirrors Player.isBlocking / getItemBlockingWith: using a
        // BLOCKS_ATTACKS item AND past its blockDelayTicks (shield = 5 ticks).
        // Flag only — no damage math (no combat system).
        bool isBlocking() const;

        // Slots mutated during this tick's item-use processing (hand writes,
        // finishUsingItem replacements). Drained by PlayerSession::Tick into
        // per-slot InventorySetSlotS2C broadcasts.
        std::vector<int>& dirtySlots() { return m_dirtySlots; }
        void markSlotDirty(int slotIndex) override { m_dirtySlots.push_back(slotIndex); }

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

        // Game::IUsePlayer — lets item behaviours ask about creative without
        // common code depending on Server::GameMode.
        bool isCreative() const override { return m_gameMode == GameMode::CREATIVE; }
        
        // Set flying state
        void setFlying(bool flying);
        
        // Check if player can reach position
        bool canReach(const glm::vec3& pos) const;
        
        // === GETTERS ===
        
        uint32_t getPlayerId() const { return m_playerId; }
        const std::string& getName() const { return m_name; }
        void setName(const std::string& name) { m_name = name; }
        // Stick-figure colour id (Game::PlayerColorId raw value). Set from the
        // client's LoginStart packet at join time; broadcast in PlayerInfoS2C ADD
        // so other clients render this player in the chosen colour. 0 = Default.
        uint8_t getColorId() const { return m_colorId; }
        void    setColorId(uint8_t id) { m_colorId = id; }
        
        const glm::dvec3& getPosition() const override { return m_position; }
        float getYaw() const override { return m_rotation.x; }
        float getPitch() const override { return m_rotation.y; }
        const glm::vec2& getRotation() const { return m_rotation; }
        
        int getDimensionId() const { return m_dimensionId; }
        void setDimensionId(int id) { m_dimensionId = id; }
        
        float getHealth() const { return m_health; }
        int getFood() const { return m_foodData.getFoodLevel(); }
        // Dead until PERFORM_RESPAWN — set when damage() drops health to 0.
        // While dead the session ignores move packets (the body is frozen)
        // and further damage is a no-op.
        bool isDead() const { return m_isDead; }

        // Hunger / saturation / exhaustion — mirrors Player.getFoodData().
        FoodData&       getFoodData()       { return m_foodData; }
        const FoodData& getFoodData() const { return m_foodData; }

        // XP — mirrors Player.experienceLevel / .experienceProgress. Read by
        // the furnace payout, the anvil's level cost and the enchanting table.
        PlayerExperience&       getExperience()       { return m_experience; }
        const PlayerExperience& getExperience() const { return m_experience; }

        // Mirrors Player.canEat(canAlwaysEat) — canAlwaysEat || needsFood().
        bool canEat(bool canAlwaysEat) const {
            return canAlwaysEat || m_foodData.needsFood();
        }
        
        GameMode getGameMode() const { return m_gameMode; }
        bool isFlying() const { return m_flying; }
        bool canFly() const { return m_canFly; }

        // Fall-distance tracking (driven from PlayerSession::HandlePlayerMove
        // off the client's move packets — the server doesn't simulate the fall).
        float getFallDistance() const { return m_fallDistance; }
        void  addFallDistance(float d) { m_fallDistance += d; }
        void  resetFallDistance() { m_fallDistance = 0.0f; }
        
        bool isOnGround() const { return m_onGround; }
        void setOnGround(bool onGround) { m_onGround = onGround; }

        // MC Entity.isSprinting. Client-authoritative here (movement is), and
        // recorded because Player.canCriticalAttack excludes a sprinting
        // player — a sprint-hit is a KNOCKBACK attack in MC, never a crit.
        bool isSprinting() const { return m_sprinting; }
        void setSprinting(bool v) { m_sprinting = v; }

        // MC LivingEntity.getKnownMovement, horizontal component, in blocks per
        // tick. Read by the sweep-attack check (Player.isSweepAttack), which
        // refuses to sweep when the attacker is moving faster than walking
        // pace. Written from the move-packet delta, one packet per client tick.
        double getKnownHorizontalMovement() const { return m_knownHorizontalMovement; }
        void   setKnownHorizontalMovement(double v) { m_knownHorizontalMovement = v; }

        // Monotonic count of landed damage events — see the bump in damage().
        uint32_t getDamageCounter() const { return m_damageCounter; }

        // ── Attack strength (MC Player.attackStrengthTicker) ───────────────
        //
        // The cooldown that makes 1.9+ combat what it is: swinging before the
        // bar refills scales the damage down hard, and only a full-strength hit
        // can crit or sweep.
        int  getAttackStrengthTicker() const { return m_attackStrengthTicker; }
        void resetAttackStrengthTicker() { m_attackStrengthTicker = 0; }

        // 20 / ATTACK_SPEED, in ticks. An iron sword's 1.6/s is 12.5 ticks.
        float getCurrentItemAttackStrengthDelay() const;
        // MC clamps to [0,1]; `adjust` is the half-tick MC adds when attacking
        // so a hit landing on the exact boundary counts as full strength.
        float getAttackStrengthScale(float adjust) const;
        
        bool IsSneaking() const override { return m_sneaking; }
        void setSneaking(bool sneaking) { m_sneaking = sneaking; }
        
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
        uint8_t     m_colorId = 0; // Game::PlayerColorId::Default
        // TODO: UUID m_uuid;
        // TODO: ProfileProperties m_profile; // skin data
        // TODO: PermissionLevel m_permissions;
        
        // === TRANSFORM & PHYSICS ===
        glm::dvec3 m_position{0.0, 67.0, 0.0};
        glm::vec2 m_rotation{0.0f, 0.0f}; // yaw, pitch
        glm::vec3 m_velocity{0.0f};
        bool m_onGround = true;
        bool m_sprinting = false;
        double m_knownHorizontalMovement = 0.0;
        int  m_attackStrengthTicker = 0;
        // MC Player.lastItemInMainHand — the ticker resets when the held item
        // CHANGES KIND (ItemStack.isSameItem), so pulling a fresh sword out of
        // the hotbar never hands you a charged swing. Count is deliberately not
        // part of it: MC compares the item, not the stack size.
        uint32_t m_lastItemInMainHand = 0;
        bool m_sneaking = false;
        // TODO: AABB m_boundingBox;
        int m_dimensionId = 0;
        // TODO: glm::vec3 m_respawnPoint;
        
        // === ATTRIBUTES & STATUS ===
        float m_health = 20.0f;
        bool  m_isDead = false;
        uint32_t m_damageCounter = 0;
        // Hunger/saturation/exhaustion — MC FoodData port (FoodData.hpp).
        FoodData m_foodData;
        // XP — MC Player's experienceLevel / experienceProgress /
        // totalExperience, with the level-curve arithmetic (PlayerExperience.hpp).
        PlayerExperience m_experience;
        // TODO: std::vector<StatusEffect> m_effects;
        float m_stepHeight = 0.6f;
        float m_fallDistance = 0.0f;
        // Set by teleport(); setPosition() bypasses the anti-cheat
        // distance check while this is in the future. Without it, a
        // portal teleport that fires server-side races against the
        // client's predicted-position move packets — the client sends
        // a move at the post-teleport location BEFORE the server's
        // teleport() runs (or before its broadcast acks back), and
        // setPosition() sees a 200+ block jump and rejects every one
        // of those moves with "moved too fast".
        std::chrono::steady_clock::time_point m_teleportGraceUntil{};
        
        // === ABILITIES & MODE ===
        GameMode m_gameMode = GameMode::SURVIVAL;
        bool m_canFly = false;
        bool m_flying = false;
        bool m_instabuild = false; // creative instant break
        float m_reachDistance = 5.0f;
        
        // === INVENTORY ===
        // 46-slot MC-compatible inventory (crafting + armor + main + hotbar + offhand).
        Game::Inventory m_inventory;
        // The always-present player menu (cursor + QUICK_CRAFT drag state live
        // on it). Declared AFTER m_inventory so the menu's slots are built over
        // a fully-constructed inventory.
        Game::InventoryMenu m_inventoryMenu{&m_inventory};
        // The block container currently open, if any (MC swaps containerMenu to
        // a CraftingMenu/ChestMenu on open and restores it on close). Owned
        // here because a table's grid exists only while its menu does.
        std::unique_ptr<Game::AbstractContainerMenu> m_openContainerMenu;
        Game::MenuType m_openMenuType = Game::MenuType::Inventory;
        // The menu on top — m_openContainerMenu when one is open, otherwise the
        // player's own.
        Game::AbstractContainerMenu* m_containerMenu = &m_inventoryMenu;
        std::optional<PendingMenuOpen>     m_pendingMenuOpen;
        std::optional<PendingCampfireFood> m_pendingCampfireFood;
        // TODO: ItemStack m_mainHand;
        // TODO: ItemStack m_offHand;
        // TODO: std::array<ItemStack, 4> m_armor;
        
        // === GAMEPLAY TIMERS ===
        int m_invulnerabilityTicks = 0;
        // TODO: int m_portalCooldown = 0;
        // TODO: int m_attackCooldown = 0;
        // TODO: bool m_sleeping = false;

        // === ITEM USE STATE — mirrors LivingEntity's useItem fields ===
        Game::ItemStack m_useItem{};              // LivingEntity.useItem
        int             m_useItemRemaining = 0;   // LivingEntity.useItemRemaining
        uint32_t        m_usedItemHand     = 0;   // flag bit 2 in MC (:3250-3252)
        bool            m_isUsingItem      = false; // flag bit 1 in MC (:3246-3248)
        // Slots mutated by item-use processing this tick (see dirtySlots()).
        std::vector<int> m_dirtySlots;

        // Per-tick countdown — called from tick(). LivingEntity.java:3254-3264.
        void updatingUsingItem();
        // One countdown step + onUseTick hook. LivingEntity.java:3296-3302.
        void updateUsingItem();
        // Consume-completion: run the item's finish behaviour and replace the
        // hand stack with the result. ItemStack.finishUsingItem →
        // Item.finishUsingItem (Item.java:221-224).
        Game::ItemStack finishUsingItem(Game::ItemStack& stack);
        
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