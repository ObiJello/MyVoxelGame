// File: src/server/player/ServerPlayer.hpp
#pragma once

#include <algorithm>
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
#include "common/entity/Morph.hpp"
#include "common/inventory/InventoryMenu.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/world/portal/PortalState.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "FoodData.hpp"
#include "PlayerExperience.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/PlayerMovementSounds.hpp"

namespace Game {
    class World;
    class Entity;
    class LivingEntity;
}

namespace Server {

    // The name an offline player gets when the launcher supplied none.
    //
    // It is not cosmetic: playerdata/<uuid>.dat is named by the type-3 UUID of
    // "OfflinePlayer:" + this string, so changing it orphans every existing
    // player file (the old one stays on disk, unread). It is also the name a
    // world carries into real Minecraft — "Notch" resolves to a real offline
    // profile there, so a world saved here loads with the same player.
    inline constexpr const char* kDefaultPlayerName = "Notch";


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
        // MC DamageTypes.GENERIC_KILL — /kill. In BYPASSES_INVULNERABILITY,
        // so creative and the hurt cooldown do not stop it.
        GENERIC_KILL,
        FALL,
        FIRE,
        LAVA,       // MC DamageTypes.LAVA — swimming in it
        DROWNING,
        STARVATION,
        VOID_DAMAGE,
        EXPLOSION,
        ENTITY_ATTACK,
        MAGIC,
        // MC DamageTypes.FALLING_BLOCK / FALLING_ANVIL / FALLING_STALACTITE /
        // STALAGMITE — a block landed on you, or you landed on one. Distinct
        // from FALL, which is you hitting the ground.
        FALLING_BLOCK,
        FALLING_ANVIL,
        FALLING_STALACTITE,
        STALAGMITE,
        // MC DamageTypes.WITHER — the WITHER effect's tick (armor-bypassing
        // like MAGIC, but its own death message).
        WITHER,
    };

    // MC CombatTracker.getDeathMessage + DamageSource.getLocalizedDeathMessage,
    // collapsed to what this port can actually distinguish.
    //
    // MC picks "death.attack.<msgId>" from the killing DamageSource, using the
    // 2-argument form when there is a causing entity and the 1-argument form
    // when there is not. The strings below are the en_us.json values for the
    // msgIds our DamageSource enum maps onto, verbatim.
    //
    // ENTITY_ATTACK with no identifiable killer falls back to
    // "death.attack.generic" rather than "death.attack.mob": the latter is
    // "%1$s was slain by %2$s", and with nothing to put in %2$s MC would never
    // reach it — it always has a causing entity for that source.
    inline std::string BuildDeathMessage(const std::string& victim,
                                         DamageSource source,
                                         const std::string& attackerName) {
        using DS = DamageSource;
        switch (source) {
            case DS::FALL:         return victim + " hit the ground too hard";
            case DS::FIRE:         return victim + " burned to death";
            case DS::LAVA:         return victim + " tried to swim in lava";
            case DS::DROWNING:     return victim + " drowned";
            case DS::STARVATION:   return victim + " starved to death";
            case DS::VOID_DAMAGE:  return victim + " fell out of the world";
            case DS::EXPLOSION:    return victim + " blew up";
            // MC death.attack.magic, or death.attack.indirectMagic when the
            // magic has a causing entity (a witch's harming potion).
            case DS::MAGIC:
                if (!attackerName.empty()) {
                    return victim + " was killed by " + attackerName + " using magic";
                }
                return victim + " was killed by magic";
            case DS::WITHER:       return victim + " withered away";   // death.attack.wither
            case DS::GENERIC_KILL: return victim + " was killed";   // death.attack.genericKill
            // MC death.attack.fallingBlock / fallingStalactite / stalagmite.
            case DS::FALLING_BLOCK:      return victim + " was squashed by a falling block";
            case DS::FALLING_ANVIL:      return victim + " was squashed by a falling anvil";
            case DS::FALLING_STALACTITE: return victim + " was skewered by a falling stalactite";
            case DS::STALAGMITE:         return victim + " was impaled on a stalagmite";
            case DS::ENTITY_ATTACK:
                if (!attackerName.empty()) {
                    return victim + " was slain by " + attackerName;
                }
                return victim + " died";
            case DS::GENERIC:
            default:               return victim + " died";
        }
    }

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
            // The trading mob for MenuType::Merchant (an entity-backed menu
            // has no block position); -1 otherwise.
            int32_t        entityId = -1;
        };
        void OpenMenu(Game::MenuType type, const glm::ivec3& pos) override {
            m_pendingMenuOpen = PendingMenuOpen{type, pos, -1};
        }
        // MC Merchant.openTradingScreen → player.openMenu(MerchantMenu).
        void OpenMerchantMenu(int32_t merchantEntityId) {
            m_pendingMenuOpen = PendingMenuOpen{Game::MenuType::Merchant, glm::ivec3(0), merchantEntityId};
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
        // IUsePlayer::CreateFilledResult — MC ItemUtils.createFilledResult.
        // Overflow that the inventory cannot take is queued here (MC
        // player.drop) for PlayerSession::FlushPendingDrops, which owns the
        // item-entity manager; the dispatch itself has no world.
        void CreateFilledResult(Game::ItemStack& held, const Game::ItemStack& filled) override;
        // Queue an item the inventory could not take (MC player.drop) for
        // PlayerSession::FlushPendingDrops.
        void queuePendingDrop(const Game::ItemStack& stack) { m_pendingDrops.push_back(stack); }
        // Items to drop in front of the player once the dispatch returns.
        std::vector<Game::ItemStack> takePendingDrops() {
            std::vector<Game::ItemStack> out;
            out.swap(m_pendingDrops);
            return out;
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
        // MC LivingEntity.kill → hurtServer(genericKill, Float.MAX_VALUE).
        // generic_kill BYPASSES_INVULNERABILITY: neither creative abilities
        // nor the hurt cooldown stop it, only being dead already.
        void kill();
        // Overload carrying the killer's display name, for the death message.
        // MC gets this from DamageSource.causingEntity via CombatTracker; we
        // have no combat tracker, so the caller passes it at the one site that
        // knows (PlayerEntityView::ActuallyHurt).
        void damage(float amount, DamageSource source, const std::string& attackerName);

        // What killed this player, for the death broadcast. MC reads the
        // equivalent off the CombatTracker's last CombatEntry.
        DamageSource       getLastDamageSource() const { return m_lastDamageSource; }
        const std::string& getLastAttackerName() const { return m_lastAttackerName; }
        
        // Heal player — MC LivingEntity.heal: clamped to getMaxHealth().
        void heal(float amount);

        // MC LivingEntity.getMaxHealth: the MAX_HEALTH attribute, 20 plus
        // HEALTH_BOOST's +4 per level.
        float getMaxHealth() const;
        // MC Player's DATA_PLAYER_ABSORPTION_ID (the golden hearts). Set
        // through the view (ABSORPTION's onEffectStarted); the damage path
        // soaks hits with it first.
        float getAbsorptionAmount() const { return m_absorptionAmount; }
        void  setAbsorptionAmount(float v) { m_absorptionAmount = v < 0.0f ? 0.0f : v; }
        // MC getMaxAbsorption — MAX_ABSORPTION, ABSORPTION's +4 per level.
        float getMaxAbsorption() const;
        // MC onAttributeUpdated for MAX_HEALTH / MAX_ABSORPTION: clamp health
        // and absorption after an effect that raised them ends.
        void clampToEffectMaxima();
        // MC Attributes.SAFE_FALL_DISTANCE: 3 + JUMP_BOOST's +1 per level.
        float getSafeFallDistance() const;
        // MC LivingEntity.checkTotemDeathProtection — a totem of undying in
        // either hand (main first) is spent to survive a killing blow that
        // is not #bypasses_invulnerability. Called by damage() at 0 health.
        bool checkTotemDeathProtection(DamageSource source);
        // MC Player.getLuck — the LUCK attribute: base 0, LUCK +1 and UNLUCK
        // -1 per level. Read by container loot (LootParams.withLuck).
        float getLuck() const;
        // MC Attributes.ATTACK_DAMAGE: the base 1.0 plus the held weapon's
        // modifier, then STRENGTH (+3/level) and WEAKNESS (-4/level).
        float getAttackDamage(float itemDamage) const;
        // MC Player.isMobilityRestricted — BLINDNESS (no sprint, no crits).
        bool isMobilityRestricted() const { return hasEffect(Game::MobEffectId::Blindness); }

        // ── Status effects (MC LivingEntity.activeEffects, on the player) ──
        //
        // The list LIVES here — the ServerPlayer is what survives a dimension
        // change and what is saved (playerdata "active_effects") — but every
        // MC hook runs through the player's PlayerEntityView, the
        // LivingEntity whose EffectStorage() is this list: its AddEffect is
        // MC's addEffect (update rules, onEffectStarted, attribute modifiers,
        // the UpdateMobEffect packet), its TickEffects MC's tickEffects.
        // These helpers route there; they return false when the player has
        // no live view (between levels), which no gameplay path hits.
        std::vector<Game::MobEffectInstance>&       activeEffects()       { return m_activeEffects; }
        const std::vector<Game::MobEffectInstance>& activeEffects() const { return m_activeEffects; }
        bool hasEffect(Game::MobEffectId id) const { return Game::HasEffectIn(m_activeEffects, id); }
        const Game::MobEffectInstance* getEffect(Game::MobEffectId id) const {
            return Game::FindEffectIn(m_activeEffects, id);
        }
        // The live view (MC: the player IS this LivingEntity). Null between levels.
        Game::LivingEntity* effectEntity() const;
        bool addEffect(const Game::MobEffectInstance& effect, Game::Entity* source = nullptr);
        bool removeEffect(Game::MobEffectId id);
        bool removeAllEffects();

        // MC ServerPlayer.onEffectAdded / onEffectUpdated / onEffectsRemoved's
        // packet halves: ClientboundUpdateMobEffectPacket(id, effect, blend)
        // and ClientboundRemoveMobEffectPacket to this player's connection.
        void sendEffectUpdate(const Game::MobEffectInstance& effect, bool blend) const;
        void sendEffectRemove(Game::MobEffectId id) const;
        // MC PlayerList.sendActivePlayerEffects — every active effect,
        // blend=false, on join and on a level change.
        void sendAllEffects() const;

        // ── Armor (MC LivingEntity.getArmorValue / the ARMOR attribute) ──
        // Summed from the four armor slots' pieces (GeneratedItemAttributes'
        // ArmorMaterial rows). Read by damage() for the sources armor
        // applies to; the HUD reads the same sum off the client's inventory.
        float getArmorValue() const;
        float getArmorToughness() const;
        // MC DamageTypeTags.BYPASSES_ARMOR, on this port's sources.
        static bool bypassesArmor(DamageSource source);

        // /gamerule shared_vitals: this player takes the pool's values. A
        // drop in health is a hit (the hurt flash and cooldown, as damage()
        // gives them); a pool at zero kills, with the source and killer of
        // whoever brought it there so the death message reads right.
        void applySharedVitals(float health, int foodLevel, float saturation, float exhaustion,
                               DamageSource deathSource, const std::string& deathAttacker);
        
        // === ABILITIES ===
        
        // Set game mode
        void setGameMode(GameMode mode);

        // Game::IUsePlayer — lets item behaviours ask about creative without
        // common code depending on Server::GameMode.
        bool isCreative() const override { return m_gameMode == GameMode::CREATIVE; }

        // MC Player.displayClientMessage — routed through the session manager
        // rather than held as a connection pointer, because a ServerPlayer
        // outlives its connection across a reconnect.
        void DisplayClientMessage(const std::string& text, bool actionBar) override;
        
        // Set flying state
        void setFlying(bool flying);
        
        // Check if player can reach position
        bool canReach(const glm::vec3& pos) const;
        // MC Player.blockInteractionRange: the BLOCK_INTERACTION_RANGE
        // attribute — 4.5 by default, +0.5 in creative (ServerPlayer's
        // CREATIVE_BLOCK_INTERACTION_RANGE_MODIFIER). For callers that
        // measure from a different eye (a portal's image of it).
        float getReachDistance() const {
            return m_reachDistance + (m_gameMode == GameMode::CREATIVE ? 0.5f : 0.0f);
        }
        // MC Player.isWithinBlockInteractionRange(pos, buffer): the distance
        // from `eye` to the nearest point ON the block's box, against reach
        // plus `buffer`. The break handler passes 1.0 — that slack is what
        // lets a block whose FACE is in reach but whose centre is not be
        // broken; measuring to the centre instead is what made a dig at
        // the edge of reach snap back a moment later.
        bool isWithinBlockInteractionRange(const glm::dvec3& eye, const glm::ivec3& pos,
                                           double buffer) const;
        // The same test from this player's own eye, with MC's 1.0 buffer —
        // what handleBlockBreakAction and handleUseItemOn both apply.
        bool canReachBlock(const glm::ivec3& pos) const;
        
        // === GETTERS ===
        
        uint32_t getPlayerId() const { return m_playerId; }
        const std::string& getName() const { return m_name; }
        std::string getPlainTextName() const override { return m_name; }
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
        
        int getDimensionId() const override { return m_dimensionId; }
        void setDimensionId(int id) {
            if (id != m_dimensionId) ++m_dimensionEpoch;
            m_dimensionId = id;
        }

        // Bumped on every dimension change. A level's PlayerEntityView
        // records it when built: a view whose number is behind was left
        // over from an earlier visit (a level nobody stands in is not
        // ticked, so SyncPlayerViews never got to drop it) and must be
        // rebuilt, not resumed — MC recreates the entity on every change.
        uint32_t getDimensionEpoch() const { return m_dimensionEpoch; }

        // MC ServerPlayer.isChangingDimension: set by a cross-dimension
        // teleport (PortalTravel), cleared by the client's ack of the
        // position packet that went with it (ServerConnection::
        // AcceptTeleportation = MC handleAcceptTeleportPacket ->
        // hasChangedDimension). While set, the portal cooldown does not run
        // (MC ServerPlayer.processPortalCooldown), so the hand-off's network
        // round trip is not spent out of the arrival cooldown.
        bool isChangingDimension() const { return m_changingDimension; }
        void setChangingDimension()      { m_changingDimension = true; }
        void hasChangedDimension()       { m_changingDimension = false; }

        // MC Entity.restoreFrom (Entity.java:3008-3009): the entity rebuilt in
        // the destination level inherits `portalCooldown` and `portalProcess`
        // from the one that left. Our "entity" for a player is a
        // PlayerEntityView owned by ServerLevelBridge, and there is one PER
        // LEVEL — crossing a portal destroys the source level's view and
        // SyncPlayerViews builds a fresh one in the destination. ServerPlayer
        // is the object that actually survives the crossing, so it is where
        // the state is parked: PortalTravel writes it just before the
        // dimension flips and PlayerEntityView's constructor reads it back.
        //
        // Dropping it is not cosmetic. The arriving player is standing INSIDE
        // the destination portal by construction, and the cooldown is the only
        // thing stopping that portal from firing again — PortalState::
        // SetAsInsidePortal re-arms it every tick you remain in the block. A
        // fresh view arrives with cooldown 0, so the exit portal fires on the
        // next tick and sends the player straight back.
        Game::PortalState&       portalState()       { return m_portalState; }
        const Game::PortalState& portalState() const { return m_portalState; }


        float getHealth() const { return m_health; }

        // Restore health from a save, bypassing damage()/heal() and every
        // side effect they carry (hurt animation, death handling, the
        // attacker bookkeeping). Loading a player is not an event in the
        // world, so nothing downstream should observe it as one.
        void setHealthDirect(float health) { m_health = health; }
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

        // ── Sound ────────────────────────────────────────────────────────
        // The replay of this player's reported movement that other players
        // hear (footsteps, swimming, splashes — PlayerSession::
        // UpdateMovementStats), and the random their pitches come from (MC
        // Entity.random). The Game::World this player stands in, for the
        // sounds ServerPlayer plays itself (hurt, death, level-up, equip).
        Game::PlayerMovementSounds& movementSounds() { return m_movementSounds; }
        Game::JavaRandom&           soundRandom()    { return m_soundRandom; }
        Game::World*                soundWorld() const;

        // Mirrors Player.canEat(canAlwaysEat) — canAlwaysEat || needsFood().
        bool canEat(bool canAlwaysEat) const {
            return canAlwaysEat || m_foodData.needsFood();
        }
        
        GameMode getGameMode() const { return m_gameMode; }
        bool isFlying() const { return m_flying; }
        // The player's size (1 = vanilla): scaled immersive portals change
        // it; the client keeps the same number. Eye height, reach and the
        // body's box all follow it.
        float getScale() const { return m_scale; }
        void  setScale(float scale) { m_scale = std::clamp(scale, 0.05f, 32.0f); }
        // /invisible: other clients draw neither this player's body nor
        // their name tag (broadcast on every PlayerUpdateS2C). Session
        // state, not saved.
        bool  isInvisible() const { return m_invisible; }
        void  setInvisible(bool on) { m_invisible = on; }
        // /morph: what other clients draw in place of this player and whose
        // body this player takes (Game::Morph code; kNone = none), plus a
        // mob's walking speed. Rides every PlayerUpdateS2C and the
        // abilities packet. Session state, not saved.
        uint32_t getMorph()      const { return m_morph; }
        float    getMorphSpeed() const { return m_morphSpeed; }
        bool     isMorphed()     const { return !Game::Morph::IsNone(m_morph); }
        void     setMorph(uint32_t code, float walkSpeed) { m_morph = code; m_morphSpeed = walkSpeed; m_morphAnim = 0; }
        uint8_t  getMorphAnim() const { return m_morphAnim; }
        void     setMorphAnim(uint8_t a) { m_morphAnim = a; }
        float getEyeHeight() const { return Game::Morph::DimsOf(m_morph).eyeHeight * m_scale; }

        // Debug noclip. There is no vanilla equivalent — the flag exists so
        // the state survives a save and a rejoin, which is the only reason
        // the server knows about it at all. Collision is resolved entirely on
        // the client (PlayerPhysics::noclip), so this is a mirror, not an
        // authority, and gating it would buy nothing.
        bool isNoclip() const { return m_noclip; }
        void setNoclip(bool v) { m_noclip = v; }
        bool canFly() const { return m_canFly; }

        // Fall-distance tracking (driven from PlayerSession::HandlePlayerMove
        // off the client's move packets — the server doesn't simulate the fall).
        float getFallDistance() const { return m_fallDistance; }
        void  addFallDistance(float d) { m_fallDistance += d; }
        void  resetFallDistance() { m_fallDistance = 0.0f; }
        
        bool isOnGround() const { return m_onGround; }
        void setOnGround(bool onGround) { m_onGround = onGround; }

        // MC Entity.remainingFireTicks on the player: set by lava contact
        // (Entity.lavaIgnite, 15 s), counted down and paid out at 1 damage
        // per 20 ticks in tick(), put out by water. Not persisted or synced
        // yet — the client has no burning overlay to show for it.
        bool isOnFire() const { return m_remainingFireTicks > 0; }
        int  getRemainingFireTicks() const { return m_remainingFireTicks; }
        void setRemainingFireTicks(int ticks) { m_remainingFireTicks = ticks; }
        void igniteForSeconds(float seconds) {
            const int ticks = static_cast<int>(seconds * 20.0f);
            if (ticks > m_remainingFireTicks) m_remainingFireTicks = ticks;
        }
        void clearFire() { m_remainingFireTicks = 0; }

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

        // ── Bed / sleeping (MC LivingEntity.sleepingPos, Player.sleepCounter) ──
        //
        // The bed's HEAD cell while asleep; its presence IS isSleeping(), as
        // in MC. PlayerSession owns the transitions (StartSleepInBed /
        // StopSleepInBed): they need the level, the connection and the other
        // sessions, none of which the player has. tick() runs the counter.
        bool isSleeping() const { return m_sleepingPos.has_value(); }
        const std::optional<glm::ivec3>& getSleepingPos() const { return m_sleepingPos; }
        void setSleepingPos(const glm::ivec3& bedPos) { m_sleepingPos = bedPos; }
        void clearSleepingPos() { m_sleepingPos.reset(); }
        // MC Player.getSleepTimer / isSleepingLongEnough (SLEEP_DURATION = 100).
        int  getSleepTimer() const { return m_sleepCounter; }
        void setSleepTimer(int ticks) { m_sleepCounter = ticks; }
        bool isSleepingLongEnough() const { return isSleeping() && m_sleepCounter >= 100; }

        // ── Respawn point (MC ServerPlayer.RespawnConfig over LevelData.RespawnData) ──
        struct RespawnConfig {
            int        dimensionId = 0;   // Game::DimensionToRaw
            glm::ivec3 pos{0};            // the bed's head cell
            float      yaw   = 0.0f;
            float      pitch = 0.0f;
            bool       forced = false;    // /spawnpoint sets it; a bed never does
        };
        const std::optional<RespawnConfig>& getRespawnConfig() const { return m_respawnConfig; }
        // MC setRespawnPosition. Returns true when the POSITION changed — the
        // one case MC shows "Respawn point set" for (RespawnConfig
        // .isSamePosition); re-clicking your own bed says nothing.
        bool setRespawnConfig(const std::optional<RespawnConfig>& config);

        // ── The Hush: the last Hush gate crossed (docs/the-hush.md, recall
        // chime). Where the player STOOD on arriving through a hush_portal —
        // the far side's landing, in front of the gate — so the chime
        // (server/items/HushItems) can send them back to it from anywhere,
        // any dimension. Saved as `obey_hush_gate` (PlayerDataStore).
        struct HushGateMark {
            int        dimensionId = 0;   // Game::DimensionToRaw
            glm::dvec3 pos{0.0};
            float      yaw = 0.0f;
        };
        const std::optional<HushGateMark>& getLastHushGate() const { return m_lastHushGate; }
        void setLastHushGate(const std::optional<HushGateMark>& mark) { m_lastHushGate = mark; }

        // ── IUsePlayer: pending sign requests ────────────────────────────
        // A sign was clicked during use dispatch: an empty hand asks for the
        // editor (SignBlock.openTextEdit), a dye / ink / honeycomb asks to be
        // applied. The block entity and the connection are PlayerSession's,
        // so both are recorded here and drained right after the dispatch.
        struct PendingSignUse {
            glm::ivec3 pos;
            bool       applyItem;   // false: open the editor
            uint32_t   hand;
        };
        void OpenSignEditor(const glm::ivec3& pos) override {
            m_pendingSignUse = PendingSignUse{pos, false, 0};
        }
        void ApplySignItem(const glm::ivec3& pos, uint32_t hand) override {
            m_pendingSignUse = PendingSignUse{pos, true, hand};
        }
        std::optional<PendingSignUse> takePendingSignUse() {
            auto pending = m_pendingSignUse;
            m_pendingSignUse.reset();
            return pending;
        }

        // ── IUsePlayer: pending book open ─────────────────────────────────
        // MC ServerPlayer.openItemGui: a written book was used. The resolve
        // and the OpenBookS2C need the connection, so the hand is recorded
        // here and PlayerSession::FlushPendingBookOpen does the rest right
        // after the use dispatch returns.
        void OpenItemGui(Game::ItemStack& stack, uint32_t hand) override;
        std::optional<uint32_t> takePendingBookOpen() {
            auto pending = m_pendingBookOpen;
            m_pendingBookOpen.reset();
            return pending;
        }

        // ── IUsePlayer: pending bed use ───────────────────────────────────
        // A bed was right-clicked during use dispatch (IUsePlayer::UseBed).
        // Recorded for the same reason as the menu request: the sleep checks
        // read the level and the monsters, the blast needs the level, and
        // the answer goes out on the connection — all PlayerSession's.
        struct PendingBedUse {
            glm::ivec3 headPos;
            bool       destroyOnUse;
        };
        void UseBed(const glm::ivec3& headPos, bool destroyOnUse) override {
            m_pendingBedUse = PendingBedUse{headPos, destroyOnUse};
        }
        std::optional<PendingBedUse> takePendingBedUse() {
            auto pending = m_pendingBedUse;
            m_pendingBedUse.reset();
            return pending;
        }
        
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
        uint32_t m_dimensionEpoch = 0;
        bool m_changingDimension = false;
        // Survives the dimension change the PlayerEntityView does not — see
        // portalState() above.
        Game::PortalState m_portalState;
        // The bed's head cell while asleep, and MC Player.sleepCounter.
        std::optional<glm::ivec3> m_sleepingPos;
        int m_sleepCounter = 0;
        // Where death sends this player back to (MC ServerPlayer.respawnConfig).
        std::optional<RespawnConfig> m_respawnConfig;
        std::optional<HushGateMark> m_lastHushGate;
        std::optional<PendingBedUse> m_pendingBedUse;
        std::optional<PendingSignUse> m_pendingSignUse;
        std::optional<uint32_t> m_pendingBookOpen;   // OpenItemGui: the hand
        
        // === ATTRIBUTES & STATUS ===
        float m_health = 20.0f;
        bool  m_isDead = false;
        DamageSource m_lastDamageSource = DamageSource::GENERIC;
        std::string  m_lastAttackerName;   // empty = no identifiable killer
        uint32_t m_damageCounter = 0;
        // Hunger/saturation/exhaustion — MC FoodData port (FoodData.hpp).
        FoodData m_foodData;
        // XP — MC Player's experienceLevel / experienceProgress /
        // totalExperience, with the level-curve arithmetic (PlayerExperience.hpp).
        PlayerExperience m_experience;
        // MC LivingEntity.activeEffects — see activeEffects() above.
        std::vector<Game::MobEffectInstance> m_activeEffects;
        // MC Player DATA_PLAYER_ABSORPTION_ID.
        float m_absorptionAmount = 0.0f;
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
        bool m_noclip = false;
        bool m_instabuild = false; // creative instant break
        float m_reachDistance = 4.5f;   // MC Player.DEFAULT_BLOCK_INTERACTION_RANGE
        bool  m_invisible = false;
        float m_scale = 1.0f;
        uint32_t m_morph      = Game::Morph::kNone;
        float    m_morphSpeed = 0.0f;
        uint8_t  m_morphAnim  = 0;   // the client's, relayed (creeper swell)
        
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
        std::vector<Game::ItemStack>       m_pendingDrops;
        // TODO: ItemStack m_mainHand;
        // TODO: ItemStack m_offHand;
        // TODO: std::array<ItemStack, 4> m_armor;
        
        // === SOUND ===
        Game::PlayerMovementSounds m_movementSounds;
        Game::JavaRandom           m_soundRandom{static_cast<int64_t>(reinterpret_cast<uintptr_t>(this)) ^ 0x5DEECE66DLL};
        // Player.giveExperienceLevels' level-up chime: the level last seen by
        // tick() and MC's lastLevelUpTime (at most one chime per 100 ticks).
        int m_lastSeenXpLevel = -1;
        int m_lastLevelUpTick = -100000;
        // LivingEntity.onEquipItem, watched per tick: the armor last worn
        // (item ids, HEAD..FEET); false until the first tick has seen it
        // (MC's firstTick — no equip sounds for what a player logs in wearing).
        std::array<uint32_t, 4> m_lastArmorItems{};
        bool                    m_armorSeen = false;

        // === GAMEPLAY TIMERS ===
        int m_invulnerabilityTicks = 0;
        int m_remainingFireTicks = 0;
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