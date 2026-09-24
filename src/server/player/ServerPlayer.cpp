// File: src/server/player/ServerPlayer.cpp
#include "ServerPlayer.hpp"
#include "common/physics/Physics.hpp"   // AABBd — block interaction range
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/ConsumableBehavior.hpp"
#include "common/world/level/World.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/core/Log.hpp"
#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"
#include "common/sound/SoundEvents.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "common/network/packets/game/MobEffectPackets.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/data/DataComponents.hpp"
#include <algorithm>
#include <cmath>

namespace Server {

    ServerPlayer::ServerPlayer(uint32_t playerId, const std::string& name)
        : m_playerId(playerId)
        , m_name(name) {
        m_lastUpdateTime = std::chrono::steady_clock::now();

        // A new player starts with an empty inventory, as in MC (the old
        // dev-time starter hotbar of dirt/grass/lava/... is gone); a
        // returning player's inventory comes from their save.

        // m_inventoryMenu is built over m_inventory by its member initialiser;
        // only the game-mode-dependent flag needs setting here.
        m_inventoryMenu.creative = (m_gameMode == GameMode::CREATIVE);

        Log::Info("ServerPlayer: Created player %u '%s' at (%.1f, %.1f, %.1f)",
                 m_playerId, m_name.c_str(), m_position.x, m_position.y, m_position.z);
    }

    ServerPlayer::~ServerPlayer() {
        // A player who disconnects with a crafting table open would otherwise
        // take its grid contents with them — the menu owns that storage and is
        // about to be destroyed. MC's disconnect path calls doCloseContainer
        // for the same reason. Safe here: m_openContainerMenu is still fully
        // alive, and m_inventory outlives it (declared earlier, destroyed later).
        closeContainerMenu();
        Log::Info("ServerPlayer: Destroyed player %u '%s'", m_playerId, m_name.c_str());
    }

    // === LIFECYCLE ===

    float ServerPlayer::getCurrentItemAttackStrengthDelay() const {
        // MC: 1 / ATTACK_SPEED * 20. The player's base is 4.0 and the held
        // weapon's modifier is ADDED to it, so a bare hand refills in 5 ticks
        // and an axe in 20-25.
        float itemDamage = 0.0f, itemSpeed = 0.0f;
        Game::GetItemAttackAttributes(
            m_inventory.GetSlot(Game::Inventory::HotbarToIndex(
                m_inventory.GetSelectedSlot())).itemId,
            itemDamage, itemSpeed);
        // The ATTACK_SPEED attribute: base 4.0 plus the weapon's ADD_VALUE,
        // then HASTE (+10%/level) and MINING_FATIGUE (-10%/level) as
        // ADD_MULTIPLIED_TOTAL.
        const float attackSpeed = static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::AttackSpeed,
            static_cast<double>(Game::kPlayerBaseAttackSpeed + itemSpeed), m_activeEffects));
        // A pathological modifier could zero this; MC would divide by zero too,
        // but a NaN delay would make every hit read as full strength.
        if (attackSpeed <= 0.0f) return 1.0e6f;
        return 20.0f / attackSpeed;
    }

    float ServerPlayer::getAttackStrengthScale(float adjust) const {
        const float delay = getCurrentItemAttackStrengthDelay();
        const float v = (static_cast<float>(m_attackStrengthTicker) + adjust) / delay;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    Game::World* ServerPlayer::soundWorld() const {
        IntegratedServer* server = g_integratedServer.get();
        ServerLevel* level = server ? server->GetLevel(Game::DimensionFromRaw(m_dimensionId)) : nullptr;
        return level ? level->World() : nullptr;
    }

    void ServerPlayer::tick(Game::World* world, int currentTick) {
        // MC Player.tick: the ticker counts up every tick and is reset by an
        // attack or a change of held item.
        ++m_attackStrengthTicker;

        // ── MC Player.giveExperienceLevels' chime ─────────────────────────
        // Every 5th level reached (amount > 0), at most once per 100 ticks:
        // PLAYER_LEVELUP for everyone (except = null), its volume growing to
        // full at level 30. The XP arithmetic lives in PlayerExperience,
        // which knows no level or tick, so the crossing is noticed here — a
        // jump over several levels in one tick chimes once, as MC's loop of
        // one-level giveExperienceLevels calls would (the 100-tick guard).
        {
            const int level = m_experience.Level();
            if (m_lastSeenXpLevel >= 0 && level > m_lastSeenXpLevel) {
                bool crossedFive = false;
                for (int l = m_lastSeenXpLevel + 1; l <= level; ++l) crossedFive |= (l % 5 == 0);
                if (crossedFive && static_cast<float>(m_lastLevelUpTick) < static_cast<float>(currentTick) - 100.0f) {
                    if (Game::World* w = world ? world : soundWorld()) {
                        const float vol = level > 30 ? 1.0f : static_cast<float>(level) / 30.0f;
                        w->PlaySound(nullptr, m_position, Game::SoundEvents::PLAYER_LEVELUP,
                                     Game::SoundSource::Players, vol * 0.75f, 1.0f);
                    }
                    m_lastLevelUpTick = currentTick;
                }
            }
            m_lastSeenXpLevel = level;
        }

        // ── MC LivingEntity.onEquipItem's equip sound ─────────────────────
        // Whatever put it there — a right-click, the inventory screen, a
        // dispenser — a new wearable in its own armor slot sounds its
        // Equippable.equipSound for everyone (except = null). Not on the
        // first tick (what the player logs in wearing), not for spectators.
        {
            static constexpr Game::EquipmentSlot kArmor[4] = {
                Game::EquipmentSlot::HEAD, Game::EquipmentSlot::CHEST,
                Game::EquipmentSlot::LEGS, Game::EquipmentSlot::FEET};
            for (int i = 0; i < 4; ++i) {
                const Game::ItemStack& worn = m_inventory.GetSlot(Game::InventoryIndexFor(kArmor[i]));
                const uint32_t id = worn.IsEmpty() ? 0u : static_cast<uint32_t>(worn.itemId);
                if (m_armorSeen && id != m_lastArmorItems[static_cast<size_t>(i)] && id != 0 &&
                    m_gameMode != GameMode::SPECTATOR) {
                    if (auto equippable = worn.get(Game::DataComponents::EQUIPPABLE);
                        equippable && equippable->slot == kArmor[i]) {
                        if (Game::World* w = world ? world : soundWorld()) {
                            w->PlaySound(nullptr, m_position, equippable->equipSound,
                                         Game::SoundSource::Players, 1.0f, 1.0f);
                        }
                    }
                }
                m_lastArmorItems[static_cast<size_t>(i)] = id;
            }
            m_armorSeen = true;
        }

        // MC Player.tick:259-266 — resetAttackStrengthTicker() when the
        // main-hand item is no longer `isSameItem` as last tick. Without this
        // the server keeps charging through a hotbar switch while the client
        // (which does reset) shows an empty bar, so the two disagree about how
        // much damage the next swing deals.
        const uint32_t mainHand = static_cast<uint32_t>(
            m_inventory.GetSlot(Game::Inventory::HotbarToIndex(
                m_inventory.GetSelectedSlot())).itemId);
        if (mainHand != m_lastItemInMainHand) {
            m_lastItemInMainHand = mainHand;
            resetAttackStrengthTicker();
        }

        // TODO: Update invulnerability timer
        if (m_invulnerabilityTicks > 0) {
            m_invulnerabilityTicks--;
        }

        // MC Player.tick:228-248 — the sleep clock. Clamped at 100 in bed
        // (isSleepingLongEnough, the night can be skipped past it), and once
        // up it counts on to 110 and resets (the client's fade-out runs off
        // the same numbers). The rest of that block — getting up when the
        // bed rule stops allowing sleep — is PlayerSession::TickSleep, which
        // has the level.
        if (isSleeping()) {
            if (++m_sleepCounter > 100) m_sleepCounter = 100;
        } else if (m_sleepCounter > 0) {
            if (++m_sleepCounter >= 110) m_sleepCounter = 0;
        }

        // Void damage — MC Entity.checkBelowWorld: 64 blocks below the world
        // floor (minY -64 → threshold -128) deals 4/hit until death. The
        // invulnerability window rate-limits it; creative/spectator are
        // immune inside damage().
        if (!m_isDead && m_position.y < -128.0) {
            damage(4.0f, DamageSource::VOID_DAMAGE);
        }

        // ── Fluid contact (MC Entity.updateFluidInteraction + baseTick's
        //    fire block, for the player) ─────────────────────────────────
        //
        // The client owns the player's movement, but lava hurting and fire
        // burning are server authority like every other damage source. Same
        // scan MC's EntityFluidInteraction does: every cell the body box
        // overlaps, a fluid counts when its surface stands above the feet.
        bool inWater = false;
        bool inLava  = false;
        if (world && !m_isDead && m_gameMode != GameMode::SPECTATOR) {
            const double halfW = 0.3 * m_scale - 0.001;
            const double h     = 1.8 * m_scale;
            const double loY   = m_position.y + 0.001;
            const int x0 = static_cast<int>(std::floor(m_position.x - halfW));
            const int x1 = static_cast<int>(std::ceil (m_position.x + halfW)) - 1;
            const int y0 = static_cast<int>(std::floor(loY));
            const int y1 = static_cast<int>(std::ceil (m_position.y + h - 0.001)) - 1;
            const int z0 = static_cast<int>(std::floor(m_position.z - halfW));
            const int z1 = static_cast<int>(std::ceil (m_position.z + halfW)) - 1;
            for (int x = x0; x <= x1 && !(inWater && inLava); ++x) {
                for (int y = y0; y <= y1; ++y) {
                    for (int z = z0; z <= z1; ++z) {
                        const Game::FluidState fs = Game::GetFluidState(*world, x, y, z);
                        if (fs.IsEmpty()) continue;
                        const double top = static_cast<double>(y) +
                                           Game::FluidHeight(*world, glm::ivec3(x, y, z), fs);
                        if (top < loY) continue;
                        if (fs.Is(Game::FluidType::Water)) inWater = true;
                        else if (fs.Is(Game::FluidType::Lava)) inLava = true;
                    }
                }
            }
        }
        if (inWater && isOnFire()) {
            clearFire();                                     // WaterFluid.entityInside: EXTINGUISH
        }
        if (inLava) {
            // LavaFluid.entityInside: LAVA_IGNITE then lavaHurt — 15 s of
            // fire and 4 damage, the latter paced by the hurt cooldown.
            igniteForSeconds(15.0f);
            damage(4.0f, DamageSource::LAVA);
        }
        if (m_remainingFireTicks > 0) {
            // Entity.baseTick: one damage every 20 ticks while burning,
            // except while in lava (which is already hurting).
            if (m_remainingFireTicks % 20 == 0 && !inLava) {
                damage(1.0f, DamageSource::FIRE);
            }
            --m_remainingFireTicks;
        }
        
        // Status effects tick on the PlayerEntityView (MC LivingEntity
        // .tickEffects on the player — see PlayerEntityView::TickCombatState,
        // which runs TickEffects over this player's list). Here only the
        // safety net for MC's onAttributeUpdated: health and absorption never
        // stand above the maxima the current effects allow.
        clampToEffectMaxima();
        
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
            const bool peaceful = Server::g_integratedServer &&
                                  Server::g_integratedServer->GetDifficulty() == 0;
            m_foodData.tick(*this, peaceful);
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
        // An item's own finishUsingItem override (the Hush's recall chime)
        // answers instead of the CONSUMABLE path; it mutates in place.
        if (auto finish = Game::ItemRegistry::Get(stack.itemId).finishUsing) {
            finish(*this, stack);
            return stack;
        }
        return Game::ConsumableBehavior::FinishUsing(*this, stack);
    }

    // Mirrors LivingEntity.releaseUsingItem (LivingEntity.java:3426-3437).
    void ServerPlayer::releaseUsingItem() {
        Game::ItemStack& inHand = getItemInHand(m_usedItemHand);
        if (!m_useItem.IsEmpty() && inHand.itemId == m_useItem.itemId) {   // :3428
            m_useItem = inHand;   // :3429
            // :3430 useItem.releaseUsing(level, this, remaining) — per-item
            // release hook (Bow fires here). Default is a no-op
            // (Item.java:324-326); the bows set one (ItemBehaviors.cpp).
            if (auto release = Game::ItemRegistry::Get(inHand.itemId).releaseUsing) {
                release(*this, inHand, m_useItemRemaining);
                markSlotDirty(handSlotIndex(m_usedItemHand));
            }
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

    bool ServerPlayer::setRespawnConfig(const std::optional<RespawnConfig>& config) {
        // MC RespawnConfig.isSamePosition: the same GlobalPos (dimension +
        // block), yaw and pitch not compared.
        const bool samePosition = config && m_respawnConfig &&
                                  config->dimensionId == m_respawnConfig->dimensionId &&
                                  config->pos == m_respawnConfig->pos;
        const bool changed = config.has_value() && !samePosition;
        m_respawnConfig = config;
        return changed;
    }

    void ServerPlayer::respawn(const glm::vec3& spawnPos) {
        Log::Info("ServerPlayer: Respawning player %u at (%.1f, %.1f, %.1f)",
                 m_playerId, spawnPos.x, spawnPos.y, spawnPos.z);
        
        // MC PlayerList.respawn builds a NEW ServerPlayer (restoreFrom with
        // restoreAll = false): no effects, no absorption, full health of the
        // fresh MAX_HEALTH. The corpse's effects normally already ended at
        // deathTime 20 (the view's triggerOnDeathMobEffects); an immediate
        // respawn gets here first, so clear them — with the remove packets,
        // because this port's client keeps its player object.
        removeAllEffects();
        if (!m_activeEffects.empty()) {
            for (const auto& e : m_activeEffects) sendEffectRemove(e.effect);
            m_activeEffects.clear();
        }
        m_absorptionAmount = 0.0f;
        m_position = glm::dvec3(spawnPos);
        m_velocity = glm::vec3(0.0f);
        m_health = 20.0f;
        m_isDead = false;
        m_foodData.setFoodLevel(20);
        m_foodData.setSaturation(5.0f);
        m_fallDistance = 0.0f;
        m_invulnerabilityTicks = 60; // 3 seconds of invulnerability
        
        // TODO: Reset inventory if keepInventory is false
        // if (!world->getGameRule("keepInventory")) {
        //     m_inventory.clear();
        // }
    }

    // === MOVEMENT & PHYSICS ===

    void ServerPlayer::CreateFilledResult(Game::ItemStack& held, const Game::ItemStack& filled) {
        // MC ItemUtils.createFilledResult(itemStack, player, newItemStack,
        // limitCreativeStackSize = true).
        if (isCreative()) {
            if (!m_inventory.HasItem(filled.itemId)) {
                if (m_inventory.AddStack(filled) > 0) m_pendingDrops.push_back(filled);
            }
            return;
        }
        held.count -= 1;                       // itemStack.consume(1, player)
        if (held.count <= 0) {
            held = filled;
            return;
        }
        Game::ItemStack rest = filled;
        rest.count = m_inventory.AddStack(filled);   // what did not fit
        if (rest.count > 0) m_pendingDrops.push_back(rest);   // player.drop(newItemStack)
    }

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
        // MC ServerGamePacketListenerImpl.handleMovePlayer: the whole
        // "moved too quickly" test sits behind the player_movement_check rule.
        if (distance > 600.0 && !inTeleportGrace &&
            Game::Rules::GetBool(Game::Rules::Id::PlayerMovementCheck) &&
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
        // Check if player can reach (MC handleBlockBreakAction: eye to the
        // block's box, 1.0 buffer)
        if (!canReachBlock(pos)) {
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
        // Check if player can reach (MC handleUseItemOn: same box test, same
        // 1.0 buffer)
        if (!canReachBlock(pos)) {
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

    void ServerPlayer::damage(float amount, DamageSource source, const std::string& attackerName) {
        m_lastAttackerName = attackerName;
        damage(amount, source);
    }

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

        // MC LivingEntity.hurtServer: FIRE_RESISTANCE refuses every #is_fire
        // source outright (on_fire, in_fire and lava) — no hurt, no cooldown.
        if ((source == DamageSource::FIRE || source == DamageSource::LAVA) &&
            hasEffect(Game::MobEffectId::FireResistance)) {
            return;
        }

        // MC Player.isInvulnerableTo: the per-source game rules — a player
        // (only a player) takes no fall / fire / drowning damage while the
        // matching rule is off. Freeze damage has no source here (no powder
        // snow).
        {
            using Game::Rules::Id;
            if (source == DamageSource::FALL     && !Game::Rules::GetBool(Id::FallDamage))     return;
            // #is_fire: on_fire, in_fire AND lava all sit behind fire_damage.
            if ((source == DamageSource::FIRE || source == DamageSource::LAVA)
                                                 && !Game::Rules::GetBool(Id::FireDamage))     return;
            if (source == DamageSource::DROWNING && !Game::Rules::GetBool(Id::DrowningDamage)) return;
        }
        
        // TODO: Apply armor reduction
        // amount = m_armor.reduceDamage(amount, source);
        
        // MC LivingEntity.actuallyHurt → getDamageAfterArmorAbsorb: worn
        // armor takes its share of anything not in BYPASSES_ARMOR
        // (CombatRules.getDamageAfterAbsorb — toughness widens the band,
        // and armor never blocks more than 80% or 20 points).
        if (!bypassesArmor(source)) {
            const float armor = getArmorValue();
            if (armor > 0.0f) {
                const float toughness = getArmorToughness();
                const float f = 2.0f + toughness / 4.0f;
                const float g = std::clamp(armor - amount / f, armor * 0.2f, 20.0f);
                amount *= (1.0f - g / 25.0f);
            }
        }

        // MC getDamageAfterMagicAbsorb: RESISTANCE takes 5/25 per level off
        // everything but #bypasses_effects (starvation) and
        // #bypasses_resistance (the void, /kill).
        if (source != DamageSource::STARVATION && source != DamageSource::VOID_DAMAGE &&
            source != DamageSource::GENERIC_KILL) {
            if (const Game::MobEffectInstance* res = getEffect(Game::MobEffectId::Resistance)) {
                const int absorbValue = (res->amplifier + 1) * 5;
                amount = std::max(amount * static_cast<float>(25 - absorbValue) / 25.0f, 0.0f);
            }
        }

        // MC Player.actuallyHurt: the absorption hearts soak the hit first.
        {
            const float originalDamage = amount;
            amount = std::max(amount - m_absorptionAmount, 0.0f);
            m_absorptionAmount = std::clamp(m_absorptionAmount - (originalDamage - amount),
                                            0.0f, getMaxAbsorption());
        }

        // Apply damage
        m_health = std::max(0.0f, m_health - amount);
        // Bumped on every landed hit so the entity view can notice damage that
        // did NOT come through it — fall, void, starvation — and still start
        // the hurt flash and the client's camera tilt. MC gets that for free by
        // routing every source through LivingEntity.hurtServer; here the two
        // paths only meet at this line.
        ++m_damageCounter;
        m_invulnerabilityTicks = 10; // 0.5 seconds
        
        Log::Info("ServerPlayer: Player %u took %.1f damage from %d (health: %.1f)",
                 m_playerId, amount, static_cast<int>(source), m_health);

        // MC LivingEntity.hurtServer: makeSound(getDeathSound()) on the fatal
        // hit (unless a totem saves it), else playHurtSound(source) — Player
        // .getHurtSound picks by the damage type's effects (BURNING → on
        // fire, DROWNING → drown, else the plain hurt). Volume 1, the voice
        // pitch (random ± 0.2). DEVIATION: sent to everyone, the hurt player
        // included; MC's own client plays its copy from the damage event
        // packet (LivingEntity.handleDamageEvent), and this engine's hurt
        // packet carries no damage type to choose the variant from.
        const auto playVoice = [this](const char* event) {
            if (Game::World* w = soundWorld()) {
                const float pitch = (m_soundRandom.NextFloat() - m_soundRandom.NextFloat()) * 0.2f + 1.0f;
                w->PlaySound(nullptr, m_position, event, Game::SoundSource::Players, 1.0f, pitch);
            }
        };

        // MC LivingEntity.hurtServer: `if (isDeadOrDying()) { if
        // (!checkTotemDeathProtection(source)) die(source); }`.
        if (m_health <= 0.0f && checkTotemDeathProtection(source)) {
            return;
        }
        if (m_health <= 0.0f) {
            playVoice(Game::SoundEvents::PLAYER_DEATH);
        } else {
            const char* hurt = Game::SoundEvents::PLAYER_HURT;
            if (source == DamageSource::FIRE || source == DamageSource::LAVA) hurt = Game::SoundEvents::PLAYER_HURT_ON_FIRE;
            else if (source == DamageSource::DROWNING)                        hurt = Game::SoundEvents::PLAYER_HURT_DROWN;
            playVoice(hurt);
        }

        if (m_health <= 0.0f) {
            // Player died. Inventory is KEPT (no dropped-item-entity system —
            // deliberate deviation from MC's default). The health=0 in the
            // next SetHealthS2C push is the client's death signal (same as
            // MC), which opens the DeathScreen; PERFORM_RESPAWN revives via
            // respawn().
            m_isDead = true;
            m_lastDamageSource = source;
            m_isBreaking = false;
            stopUsingItem();
            Log::Info("ServerPlayer: Player %u died (source %d)",
                      m_playerId, static_cast<int>(source));
        }
    }

    bool ServerPlayer::checkTotemDeathProtection(DamageSource source) {
        // MC LivingEntity.checkTotemDeathProtection. #bypasses_invulnerability
        // (out_of_world, generic_kill) cannot be cheated.
        if (source == DamageSource::VOID_DAMAGE || source == DamageSource::GENERIC_KILL) return false;

        // InteractionHand.values(): MAIN_HAND, then OFF_HAND — the first hand
        // holding a DEATH_PROTECTION item (vanilla: only the totem) spends one.
        const int hands[2] = {
            Game::Inventory::HotbarToIndex(m_inventory.GetSelectedSlot()),
            Game::Inventory::OFFHAND_BEGIN };
        bool protectedByTotem = false;
        for (const int slot : hands) {
            Game::ItemStack& stack = m_inventory.MutableSlot(slot);
            if (stack.IsEmpty() || stack.itemId != Game::Items::TotemOfUndying) continue;
            if (--stack.count <= 0) stack.Clear();   // itemStack.shrink(1)
            protectedByTotem = true;
            break;
        }
        if (!protectedByTotem) return false;

        // setHealth(1.0F), then DeathProtection.TOTEM_OF_UNDYING's effects:
        // ClearAllStatusEffects, then REGENERATION II 45 s, ABSORPTION II
        // 5 s and FIRE_RESISTANCE 40 s. (Stats, the USED_TOTEM trigger and the
        // use vibration have no systems here.) The slot change reaches the
        // client through the per-tick inventory diff.
        m_health = 1.0f;
        removeAllEffects();
        addEffect(Game::MobEffectInstance(Game::MobEffectId::Regeneration, 900, 1));
        addEffect(Game::MobEffectInstance(Game::MobEffectId::Absorption, 100, 1));
        addEffect(Game::MobEffectInstance(Game::MobEffectId::FireResistance, 800, 0));
        // level.broadcastEntityEvent(this, 35) — the totem animation and
        // particles on every client (drawing them is the client's business).
        if (Game::LivingEntity* view = effectEntity()) {
            if (Game::EntityLevel* level = view->Level()) level->BroadcastEntityEvent(*view, 35);
        }
        Log::Info("ServerPlayer: Player %u saved by a totem of undying", m_playerId);
        return true;
    }

    void ServerPlayer::kill() {
        if (m_isDead) return;
        // /kill is a GENERIC_KILL hit in MC, and the fatal hit's death cry.
        if (Game::World* w = soundWorld()) {
            const float pitch = (m_soundRandom.NextFloat() - m_soundRandom.NextFloat()) * 0.2f + 1.0f;
            w->PlaySound(nullptr, m_position, Game::SoundEvents::PLAYER_DEATH, Game::SoundSource::Players, 1.0f, pitch);
        }
        m_health = 0.0f;
        ++m_damageCounter;           // the hurt flash / camera tilt, as any hit
        m_invulnerabilityTicks = 0;
        m_isDead = true;
        m_lastDamageSource = DamageSource::GENERIC_KILL;
        m_lastAttackerName.clear();
        m_isBreaking = false;
        stopUsingItem();
        Log::Info("ServerPlayer: Player %u killed by command", m_playerId);
    }

    bool ServerPlayer::bypassesArmor(DamageSource source) {
        // MC DamageTypeTags.BYPASSES_ARMOR: on_fire, drown, starve, fall,
        // generic, generic_kill, magic, stalagmite, out_of_world … Armor
        // stands against attacks, projectiles, explosions, lava and things
        // falling on you. This port's FIRE is both in_fire and the on_fire
        // tick; the tick is the common case, so FIRE bypasses.
        switch (source) {
            case DamageSource::ENTITY_ATTACK:
            case DamageSource::EXPLOSION:
            case DamageSource::LAVA:
            case DamageSource::FALLING_BLOCK:
            case DamageSource::FALLING_ANVIL:
            case DamageSource::FALLING_STALACTITE:
                return false;
            default:
                return true;
        }
    }

    namespace {
        constexpr Game::EquipmentSlot kArmorSlots[4] = {
            Game::EquipmentSlot::HEAD, Game::EquipmentSlot::CHEST,
            Game::EquipmentSlot::LEGS, Game::EquipmentSlot::FEET };
    }

    float ServerPlayer::getArmorValue() const {
        float total = 0.0f;
        for (const Game::EquipmentSlot slot : kArmorSlots) {
            const Game::ItemStack& piece = m_inventory.GetSlot(Game::InventoryIndexFor(slot));
            if (piece.IsEmpty()) continue;
            if (const Game::ItemArmorRow* row = Game::GetItemArmorAttributes(piece.itemId)) total += row->armor;
        }
        return total;
    }

    float ServerPlayer::getArmorToughness() const {
        float total = 0.0f;
        for (const Game::EquipmentSlot slot : kArmorSlots) {
            const Game::ItemStack& piece = m_inventory.GetSlot(Game::InventoryIndexFor(slot));
            if (piece.IsEmpty()) continue;
            if (const Game::ItemArmorRow* row = Game::GetItemArmorAttributes(piece.itemId)) total += row->armorToughness;
        }
        return total;
    }

    void ServerPlayer::applySharedVitals(float health, int foodLevel, float saturation, float exhaustion,
                                         DamageSource deathSource, const std::string& deathAttacker) {
        if (m_isDead) return;
        m_foodData.setFoodLevel(std::clamp(foodLevel, 0, 20));
        m_foodData.setSaturation(std::clamp(saturation, 0.0f, static_cast<float>(m_foodData.getFoodLevel())));
        m_foodData.setExhaustion(std::max(exhaustion, 0.0f));
        health = std::clamp(health, 0.0f, getMaxHealth());
        if (health < m_health) {
            // A hit landed on the pool: this player flinches with it.
            ++m_damageCounter;
            m_invulnerabilityTicks = std::max(m_invulnerabilityTicks, 10);
        }
        m_health = health;
        if (m_health <= 0.0f) {
            m_isDead = true;
            m_lastDamageSource = deathSource;
            m_lastAttackerName = deathAttacker;
            m_isBreaking = false;
            stopUsingItem();
            Log::Info("ServerPlayer: Player %u died with the shared health (source %d)",
                      m_playerId, static_cast<int>(deathSource));
        }
    }

    void ServerPlayer::heal(float amount) {
        // Dead players don't regenerate — MC LivingEntity.heal is a no-op
        // when dead; without this, FoodData regen could quietly "revive" a
        // corpse waiting on the death screen.
        if (m_isDead) return;
        const float maxHealth = getMaxHealth();
        if (m_health < maxHealth) {
            m_health = std::min(maxHealth, m_health + amount);
            Log::Debug("ServerPlayer: Player %u healed %.1f (health: %.1f)",
                      m_playerId, amount, m_health);
        }
    }

    // ── Effect-derived attributes (MC Player.createAttributes + the effect
    //    templates, over this player's list) ──────────────────────────────

    float ServerPlayer::getMaxHealth() const {
        return static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::MaxHealth, 20.0, m_activeEffects));
    }

    float ServerPlayer::getMaxAbsorption() const {
        return static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::MaxAbsorption, 0.0, m_activeEffects));
    }

    void ServerPlayer::clampToEffectMaxima() {
        const float maxHealth = getMaxHealth();
        if (m_health > maxHealth) m_health = maxHealth;
        const float maxAbsorption = getMaxAbsorption();
        if (m_absorptionAmount > maxAbsorption) m_absorptionAmount = maxAbsorption;
    }

    float ServerPlayer::getSafeFallDistance() const {
        return static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::SafeFallDistance, 3.0, m_activeEffects));
    }

    float ServerPlayer::getLuck() const {
        return static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::Luck, 0.0, m_activeEffects));
    }

    float ServerPlayer::getAttackDamage(float itemDamage) const {
        return static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::AttackDamage,
            static_cast<double>(Game::kPlayerBaseAttackDamage + itemDamage), m_activeEffects));
    }

    // ── Status effects ─────────────────────────────────────────────────────

    Game::LivingEntity* ServerPlayer::effectEntity() const {
        return Server::g_integratedServer
            ? Server::g_integratedServer->GetPlayerEntityView(m_playerId) : nullptr;
    }

    bool ServerPlayer::addEffect(const Game::MobEffectInstance& effect, Game::Entity* source) {
        Game::LivingEntity* view = effectEntity();
        if (!view) return false;
        return view->AddEffect(Game::MobEffectInstance(effect), source);
    }

    bool ServerPlayer::removeEffect(Game::MobEffectId id) {
        Game::LivingEntity* view = effectEntity();
        return view ? view->RemoveEffect(id) : false;
    }

    bool ServerPlayer::removeAllEffects() {
        Game::LivingEntity* view = effectEntity();
        return view ? view->RemoveAllEffects() : false;
    }

    namespace {
        ServerConnection* ConnectionFor(uint32_t playerId) {
            auto* server = Server::g_integratedServer.get();
            if (!server) return nullptr;
            auto* sessions = server->GetSessionManager();
            if (!sessions) return nullptr;
            auto session = sessions->GetSessionByConnection(playerId);
            return session ? session->GetConnection() : nullptr;
        }
    }

    void ServerPlayer::sendEffectUpdate(const Game::MobEffectInstance& effect, bool blend) const {
        // MC new ClientboundUpdateMobEffectPacket(getId(), effect, blend).
        ServerConnection* connection = ConnectionFor(m_playerId);
        if (!connection) return;
        Network::UpdateMobEffectS2CPacket p;
        p.entityId  = static_cast<int32_t>(m_playerId);
        p.effectId  = static_cast<uint8_t>(effect.effect);
        p.amplifier = effect.amplifier;
        p.duration  = effect.duration;
        if (effect.ambient)  p.flags |= Network::UpdateMobEffectS2CPacket::kFlagAmbient;
        if (effect.visible)  p.flags |= Network::UpdateMobEffectS2CPacket::kFlagVisible;
        if (effect.showIcon) p.flags |= Network::UpdateMobEffectS2CPacket::kFlagShowIcon;
        if (blend)           p.flags |= Network::UpdateMobEffectS2CPacket::kFlagBlend;
        connection->SendPacket(static_cast<uint8_t>(Network::PacketId::UpdateMobEffectS2C),
                               Network::Serialization::Serialize(p));
    }

    void ServerPlayer::sendEffectRemove(Game::MobEffectId id) const {
        // MC new ClientboundRemoveMobEffectPacket(getId(), effect).
        ServerConnection* connection = ConnectionFor(m_playerId);
        if (!connection) return;
        Network::RemoveMobEffectS2CPacket p;
        p.entityId = static_cast<int32_t>(m_playerId);
        p.effectId = static_cast<uint8_t>(id);
        connection->SendPacket(static_cast<uint8_t>(Network::PacketId::RemoveMobEffectS2C),
                               Network::Serialization::Serialize(p));
    }

    void ServerPlayer::sendAllEffects() const {
        // MC PlayerList.sendActiveEffects(player, connection): blend = false.
        for (const Game::MobEffectInstance& e : m_activeEffects) sendEffectUpdate(e, false);
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
        glm::vec3 eyePos = glm::vec3(m_position) + glm::vec3(0.0f, getEyeHeight(), 0.0f);
        float distance = glm::length(pos - eyePos);
        
        return distance <= getReachDistance() * m_scale;
    }

    bool ServerPlayer::canReachBlock(const glm::ivec3& pos) const {
        const glm::dvec3 eye = m_position + glm::dvec3(0.0, getEyeHeight(), 0.0);
        return isWithinBlockInteractionRange(eye, pos, 1.0);
    }

    bool ServerPlayer::isWithinBlockInteractionRange(const glm::dvec3& eye, const glm::ivec3& pos,
                                                     double buffer) const {
        const double range = static_cast<double>(getReachDistance()) * m_scale + buffer;
        const Game::AABBd box = Game::AABBd::FromMinMax(glm::dvec3(pos), glm::dvec3(pos) + 1.0);
        return box.DistanceToSqr(eye) < range * range;
    }

    // === INTERNAL METHODS ===

    void ServerPlayer::updatePosition(Game::World* world) {
        if (!world) return;

        // Noclip is exempt for exactly the reason flying is, and leaving it out
        // is what made a noclipping player sink a couple of blocks across a
        // rejoin while a FLYING one held station.
        //
        // In both states the CLIENT owns the position outright and reports it
        // every tick. Anything integrated here is therefore either overwritten
        // by the next move packet or — in the gap between packets — a drift the
        // client never agreed to. With gravity running against a noclipping
        // player who was holding still, m_position crept downward until
        // checkCollision caught it, and whatever the drift had reached is what
        // playerdata saved. Next login placed them there, and it compounded.
        //
        // Velocity is cleared rather than left alone so a residual from before
        // the toggle cannot fire the instant noclip is switched off.
        if (m_noclip) {
            m_velocity = glm::vec3(0.0f);
            return;
        }

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

    // ─── Container menus ─────────────────────────────────────────
    void ServerPlayer::openContainerMenu(std::unique_ptr<Game::AbstractContainerMenu> menu,
                                         Game::MenuType type) {
        if (!menu) return;

        // MC AbstractContainerMenu.transferState — the cursor belongs to the
        // player, not to the menu that happens to be showing it, so it moves
        // across. Without this, opening a table while holding a stack would
        // silently swallow it.
        const Game::ItemStack carried = m_containerMenu->getCarried();
        m_containerMenu->setCarried(Game::ItemStack{});

        // A new id means every click still in flight for the old menu is
        // rejected by PlayerSession's containerId guard instead of landing on
        // whatever slot now occupies that index.
        const uint32_t nextId = m_containerMenu->containerId + 1;

        m_openContainerMenu = std::move(menu);
        m_openMenuType      = type;
        m_containerMenu     = m_openContainerMenu.get();
        m_containerMenu->containerId = nextId;
        m_containerMenu->setCarried(carried);
    }

    Game::ContainerClickResult ServerPlayer::closeContainerMenu() {
        Game::ContainerClickResult result;
        if (!m_openContainerMenu) return result;

        // MC doCloseContainer → menu.removed(player): the table's grid goes
        // back into the inventory before the menu itself disappears.
        m_openContainerMenu->Removed(result);

        const Game::ItemStack carried = m_openContainerMenu->getCarried();
        const uint32_t nextId = m_openContainerMenu->containerId + 1;

        m_openContainerMenu.reset();
        m_openMenuType  = Game::MenuType::Inventory;
        m_containerMenu = &m_inventoryMenu;
        m_containerMenu->containerId = nextId;
        m_containerMenu->setCarried(carried);
        return result;
    }

    void ServerPlayer::DisplayClientMessage(const std::string& text, bool actionBar) {
        // MC Player.displayClientMessage -> ClientboundSystemChatPacket with
        // overlay = actionBar. Position 2 is the action bar here, 1 the chat
        // box; see IntegratedServer::BroadcastSystemMessage for the mapping.
        auto* server = Server::g_integratedServer.get();
        if (!server) return;
        auto* sessions = server->GetSessionManager();
        if (!sessions) return;
        auto session = sessions->GetSessionByConnection(m_playerId);
        if (!session) return;
        auto* connection = session->GetConnection();
        if (!connection) return;

        Network::ChatMessageS2CPacket packet;
        packet.senderId = 0;
        packet.position = actionBar ? 2 : 1;
        packet.segments.push_back(Network::ChatSegmentData{
            text, 0xFFFFFFFFu, Network::ChatClickAction::None, "", ""});
        connection->SendChatMessage(packet);
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

    void ServerPlayer::OpenItemGui(Game::ItemStack& stack, uint32_t hand) {
        // MC ServerPlayer.openItemGui: only a stack carrying written content
        // opens anything on the server's side (a book and quill is the
        // client's alone — LocalPlayer.openItemGui).
        if (!stack.get(Game::DataComponents::WRITTEN_BOOK_CONTENT)) return;
        m_pendingBookOpen = hand;
    }

} // namespace Server