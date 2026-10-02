// File: src/server/player/ServerPlayer.cpp
#include "ServerPlayer.hpp"
#include "server/advancements/CriteriaTriggers.hpp"
#include <limits>
#include "common/entity/SpearItem.hpp"
#include "common/physics/Physics.hpp"   // AABBd — block interaction range
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/ConsumableBehavior.hpp"
#include "common/entity/WeaponItems.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/world/level/World.hpp"
#include "common/particle/ParticleOptions.hpp"
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
#include "common/network/packets/game/UpdateAttributesS2CPacket.hpp"
#include "common/network/packets/game/CooldownS2CPacket.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/data/DataComponents.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include <algorithm>
#include <cmath>

namespace Server {

    namespace {
        // MC ServerPlayer.die: `killer = getKillCredit(); if (killer != null)
        // killer.awardKillScore(this, source)` — Entity.awardKillScore fires
        // ENTITY_KILLED_PLAYER for the victim, and ServerPlayer.awardKillScore
        // adds PLAYER_KILLED_ENTITY when the killer is a player. The credit is
        // the blow's causing living entity (hurtServer records it as
        // lastHurtByMob / lastHurtByPlayer before die), else the last mob to
        // hurt the player within MC's 100-tick memory. (Mob deaths award from
        // LivingEntity::AwardKillCriteria; a player's death never reaches it,
        // so Adventure's root — killed_by_something — never fired.)
        void AwardDeathCriteria(ServerPlayer& victim, const Game::DamageSourceInfo& info) {
            Game::LivingEntity* view = victim.effectEntity();
            if (!view) return;
            Game::Entity* credit = dynamic_cast<Game::LivingEntity*>(info.causing);
            if (!credit) {
                if (Game::Entity* last = view->GetLastHurtByMob();
                    last && view->Level() &&
                    view->Level()->GetGameTime() - view->GetLastHurtByMobTimestamp() <= 100) {
                    credit = last;
                }
            }
            if (!credit || credit == view) return;
            CriteriaTriggers::EntityKilledPlayer(victim, *credit, info);
            if (ServerPlayer* killer = CriteriaTriggers::PlayerOf(credit); killer && killer != &victim) {
                CriteriaTriggers::PlayerKilledEntity(*killer, *view, info);
            }
        }
    } // namespace

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

        // MC rolls the enchantment seed when a save has none; a player with
        // no save at all gets one here (a returning player's comes from
        // PlayerDataStore, which replaces it).
        m_enchantmentSeed = m_soundRandom.Next(32);   // random.nextInt()

        // MC Player.createAttributes — the player's own attribute rows (a
        // returning player's saved bases and modifiers replace them).
        Game::CreatePlayerAttributes(m_attributes);

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
        // MC Player.getCurrentItemAttackStrengthDelay: 1 / ATTACK_SPEED * 20.
        // The player's base is 4.0 and the held weapon's BASE_ATTACK_SPEED
        // modifier is added to it (a bare hand refills in 5 ticks, an axe in
        // 20-25), then HASTE / MINING_FATIGUE.
        const float attackSpeed = static_cast<float>(getAttributeValue(Game::Attribute::AttackSpeed));
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

    void ServerPlayer::CauseUseVibration(const Game::ItemStack& stack, Game::GameEventId event) {
        // MC ItemStack.causeUseVibration: only when the stack's USE_EFFECTS
        // has interact_vibrations — true for every item's default
        // (UseEffects.DEFAULT) and false for the spears (Item.spear's
        // UseEffects(true, false, 1.0)).
        if (stack.IsEmpty() || !Game::GetUseEffects(stack).interactVibrations) return;
        if (Game::Entity* self = GameEventSource()) self->GameEvent(event);
    }

    Game::Entity* ServerPlayer::GameEventSource() {
        return g_integratedServer ? g_integratedServer->GetPlayerEntityView(m_playerId) : nullptr;
    }

    void ServerPlayer::tick(Game::World* world, int currentTick) {
        // MC Player.tick: the ticker counts up every tick and is reset by an
        // attack or a change of held item.
        ++m_attackStrengthTicker;

        // MC Player.tick → this.cooldowns.tick(), and LivingEntity.tick's
        // post-impulse grace countdown (the player's copy — its entity view
        // is never Tick()ed).
        m_itemCooldowns.Tick();
        m_impulseContext.Tick();
        // MC ServerPlayer.tick → wardenSpawnTracker.tick().
        m_wardenSpawnTracker.Tick();

        // /gamerule player_step_height onto the own STEP_HEIGHT row, then
        // the own attributes to this player's client whenever they changed
        // (MC ServerEntity.sendDirtyEntityData's attribute half).
        if (g_integratedServer) applyStepHeightRule(g_integratedServer->PlayerStepHeight());
        syncAttributesToClient();

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
                // MC onEquipItem's tail: doesEmitEquipEvent → gameEvent(EQUIP
                // when the new stack is equippable, else UNEQUIP) — a piece
                // taken off is heard too.
                if (m_armorSeen && id != m_lastArmorItems[static_cast<size_t>(i)] &&
                    m_gameMode != GameMode::SPECTATOR) {
                    const bool equippableNow = !worn.IsEmpty() && worn.get(Game::DataComponents::EQUIPPABLE);
                    if (Game::Entity* self = GameEventSource()) {
                        self->GameEvent(equippableNow ? Game::GameEventId::Equip : Game::GameEventId::Unequip);
                    }
                }
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
        // MC ServerPlayer.doTick: awardStat(TIME_SINCE_REST) every tick out
        // of bed (ServerStatsCounter saturates at Integer.MAX_VALUE).
        if (!isSleeping() && m_timeSinceRest < std::numeric_limits<int>::max()) ++m_timeSinceRest;

        // Void damage — MC Entity.checkBelowWorld: 64 blocks below the world
        // floor (minY -64 → threshold -128) deals 4/hit until death. The
        // invulnerability window rate-limits it; it is in
        // BYPASSES_INVULNERABILITY, so creative and spectator die too.
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
            // The body's box: the player's, or the /morph body's.
            const Game::Morph::Dims body = Game::Morph::DimsOf(m_morph);
            const double halfW = 0.5 * body.width * m_scale - 0.001;
            const double h     = body.height * m_scale;
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
            const int difficulty = Server::g_integratedServer
                                       ? Server::g_integratedServer->GetDifficulty() : 2;
            m_foodData.tick(*this, difficulty);
        }

        // TODO: Handle portal cooldown
        // if (m_portalCooldown > 0) {
        //     m_portalCooldown--;
        // }
        
        // Item-use countdown (eating, blocking, …). Mirrors MC's
        // LivingEntity.baseTick → updatingUsingItem (LivingEntity.java:3254).
        updatingUsingItem();

        // ── Elytra flight (MC LivingEntity.tick's fallFlyTicks, aiStep's
        //    updateFallFlying) ────────────────────────────────────────────
        if (m_fallFlying) {
            ++m_fallFlyTicks;
            if (!canGlide()) {
                m_fallFlying = false;
            } else {
                // Every 10 ticks the ELYTRA_GLIDE game event; every 20 a
                // point of wear on the glider (a random one when several).
                const int checkFallFlyTicks = m_fallFlyTicks + 1;
                if (checkFallFlyTicks % 10 == 0) {
                    if ((checkFallFlyTicks / 10) % 2 == 0) {
                        // Util.getRandom(slotsWithGlider, random).
                        const std::vector<Game::EquipmentSlot> slots = Game::InventoryGliderSlots(m_inventory);
                        const Game::EquipmentSlot slot = slots[static_cast<size_t>(
                            soundRandom().NextInt(static_cast<int>(slots.size())))];
                        const int index = slot == Game::EquipmentSlot::MAINHAND
                            ? Game::Inventory::HotbarToIndex(m_inventory.GetSelectedSlot())
                            : Game::InventoryIndexFor(slot);
                        Game::ItemStack& glider = m_inventory.MutableSlot(index);
                        if (!glider.IsEmpty()) {
                            Game::HurtAndBreak(glider, 1, soundRandom(), isCreative(),
                                               [this, slot](const Game::ItemStack& broken) {
                                                   OnEquippedItemBroken(broken, slot);
                                               });
                            markSlotDirty(index);
                        }
                    }
                    if (Game::LivingEntity* view = effectEntity()) {
                        view->GameEvent(Game::GameEventId::ElytraGlide);
                    }
                }
            }
        } else {
            m_fallFlyTicks = 0;
        }

        // Update position with physics (existing functionality)
        updatePosition(world);

        // Update mining progress
        if (m_isBreaking) {
            continueDestroyBlock(m_breakingPos);
        }
    }

    // === ELYTRA FLIGHT ===

    bool ServerPlayer::canGlide() const {
        // Player.canGlide: !abilities.flying && LivingEntity.canGlide.
        if (m_flying || m_onGround || isPassenger() || hasEffect(Game::MobEffectId::Levitation)) {
            return false;
        }
        // canGlideUsing over every slot: a GLIDER item worn in its
        // EQUIPPABLE slot, with wear to spare (the elytra, or anything a
        // component patch makes one).
        return !Game::InventoryGliderSlots(m_inventory).empty();
    }

    void ServerPlayer::setFallFlyingFromClient(bool fallFlying) {
        if (fallFlying) {
            // START_FALL_FLYING: tryToStartFallFlying (not already gliding,
            // canGlide, not in a liquid — the client checked the liquid), else
            // stopFallFlying.
            if (!m_fallFlying) {
                if (canGlide()) {
                    m_fallFlying = true;
                    m_fallFlyTicks = 0;
                } else {
                    m_fallFlying = false;
                }
            }
        } else {
            m_fallFlying = false;
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
            // :3333 useItem.causeUseVibration(this, ITEM_INTERACT_START).
            CauseUseVibration(m_useItem, Game::GameEventId::ItemInteractStart);
            // :3334-3336 a KINETIC_WEAPON (a spear's charge) starts a fresh
            // recentKineticEnemies table on the player's entity.
            if (Game::Spear::Kinetic(m_useItem)) {
                if (auto* self = dynamic_cast<Game::LivingEntity*>(GameEventSource())) {
                    self->BeginKineticContacts();
                }
            }
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
        // MC ServerPlayer.updateUsingItem: CriteriaTriggers.USING_ITEM on every
        // tick of use (the spyglass advancements), then the item's tick.
        if (!m_useItem.IsEmpty()) CriteriaTriggers::UsingItem(*this, m_useItem);
        // :3297 useItem.onUseTick → ItemStack.onUseTick (ItemStack.java:1060-1064)
        // — the item's own Item.onUseTick (the crossbow's loading sounds and
        // load), then the periodic consume-phase eat sound/particle.
        const Game::Item& useItemDef = Game::ItemRegistry::Get(m_useItem.itemId);
        // A KINETIC_WEAPON's tick (ItemStack.onUseTick → damageEntities) for
        // a stack whose item has no tick of its own.
        const auto onUseTick = useItemDef.onUseTick ? useItemDef.onUseTick
                             : (m_useItem.get(Game::DataComponents::KINETIC_WEAPON) ? &Game::WeaponItems::SpearUseTick
                                                                                    : nullptr);
        if (onUseTick) {
            // MC's useItem IS the stack in the hand: the tick writes the
            // live slot (a crossbow's CHARGED_PROJECTILES), not a copy.
            const int slot = handSlotIndex(m_usedItemHand);
            Game::ItemStack& inHand = m_inventory.MutableSlot(slot);
            if (inHand.itemId == m_useItem.itemId && !inHand.IsEmpty()) {
                const Game::ItemStack before = inHand;
                onUseTick(*this, inHand, m_useItemRemaining);
                if (!Game::ItemStacksMatch(before, inHand)) markSlotDirty(slot);
                m_useItem = inHand;
            }
        }
        Game::ConsumableBehavior::OnUseTick(*this, m_useItem, m_useItemRemaining);
        // :3298 — `--useItemRemaining == 0 && !useOnRelease → completeUsingItem`
        // (useOnRelease: the crossbow, whose hold only ends on release).
        if (--m_useItemRemaining == 0 && !useItemDef.useOnRelease) {
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
        // MC applyAfterUseComponentSideEffects reads USE_COOLDOWN off the
        // stack as it was BEFORE the use (the chorus fruit's 1 s rest), so
        // it is captured first and applied after the finish.
        const Game::ItemStack stackBeforeUse = stack;
        // An item's own finishUsingItem override (the Hush's recall chime)
        // answers instead of the CONSUMABLE path; it mutates in place.
        if (auto finish = Game::ItemRegistry::Get(stack.itemId).finishUsing) {
            finish(*this, stack);
            Game::ApplyUseCooldown(m_itemCooldowns, stackBeforeUse);
            return stack;
        }
        Game::ItemStack result = Game::ConsumableBehavior::FinishUsing(*this, stack);
        Game::ApplyUseCooldown(m_itemCooldowns, stackBeforeUse);
        return result;
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
            // :3431-3433 — `if (useItem.useOnRelease()) updatingUsingItem()`:
            // one more use tick at the moment of letting go, which is where a
            // crossbow drawn to exactly full charge loads.
            if (Game::ItemRegistry::Get(inHand.itemId).useOnRelease) {
                updatingUsingItem();
            }
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
        // MC LivingEntity.stopUsingItem (server): a use that was running ends
        // with useItem.causeUseVibration(this, ITEM_INTERACT_FINISH).
        if (m_isUsingItem) CauseUseVibration(m_useItem, Game::GameEventId::ItemInteractFinish);
        // The kinetic table goes with the use (recentKineticEnemies = null).
        if (m_isUsingItem && Game::Spear::Kinetic(m_useItem)) {
            if (auto* self = dynamic_cast<Game::LivingEntity*>(GameEventSource())) {
                self->EndKineticContacts();
            }
        }
        m_isUsingItem      = false;
        m_usedItemHand     = 0;
        m_useItem          = Game::ItemStack{};
        m_useItemRemaining = 0;
    }

    void ServerPlayer::startAutoSpinAttack(int ticks, float damage, uint32_t hand,
                                           const Game::ItemStack& stack) {
        // MC Player.startAutoSpinAttack: the clock, the damage, the stack;
        // server side the shoulder parrots come off and the flag goes on.
        m_autoSpinAttackTicks = ticks;
        m_autoSpinAttackDmg   = damage;
        m_autoSpinAttackHand  = hand;
        m_autoSpinAttackItem  = stack;
        m_autoSpinAttackFlag  = true;
    }

    void ServerPlayer::stopAutoSpinAttack() {
        // checkAutoSpinAttack's server tail: the flag off, the damage and the
        // stack cleared.
        m_autoSpinAttackTicks = 0;
        m_autoSpinAttackFlag  = false;
        m_autoSpinAttackDmg   = 0.0f;
        m_autoSpinAttackItem  = Game::ItemStack{};
    }

    Game::ItemStack& ServerPlayer::autoSpinAttackWeapon(Game::ItemStack& scratch) {
        Game::ItemStack& inHand = getItemInHand(m_autoSpinAttackHand);
        if (!inHand.IsEmpty() && inHand.itemId == m_autoSpinAttackItem.itemId) return inHand;
        scratch = m_autoSpinAttackItem;
        return scratch;
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
        //
        // The fresh player has no impulse in flight either (a death mid-launch
        // must not forgive the next life's first fall).
        m_impulseContext.Reset();
        removeAllEffects();
        if (!m_activeEffects.empty()) {
            for (const auto& e : m_activeEffects) sendEffectRemove(e.effect);
            m_activeEffects.clear();
        }
        m_absorptionAmount = 0.0f;
        m_position = glm::dvec3(spawnPos);
        m_movePacketBase = m_position;
        m_velocity = glm::vec3(0.0f);
        // restoreFrom(old, false): assignBaseValues keeps every attribute
        // base (/attribute … base set survives death); the permanent
        // modifiers are NOT carried (assignPermanentModifiers runs only
        // for restoreAll) — /attribute's modifiers end with the life.
        for (const Game::AttributeInstance& row : std::vector<Game::AttributeInstance>(m_attributes.All())) {
            for (const Game::AttributeModifier& mod : row.Modifiers()) {
                if (mod.permanent) {
                    m_attributes.RemoveModifier(row.GetAttribute(), static_cast<Game::ModifierId>(mod.id));
                }
            }
        }
        // setHealth(getMaxHealth()): the fresh player's MAX_HEALTH.
        m_health = getMaxHealth();
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

    bool ServerPlayer::AddItemOrDrop(const Game::ItemStack& stack) {
        if (stack.IsEmpty()) return true;
        Game::ItemStack rest = stack;
        rest.count = m_inventory.AddStack(stack);   // what did not fit
        if (rest.count > 0) m_pendingDrops.push_back(rest);
        return true;
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
        m_movePacketBase = pos;
        m_velocity = glm::vec3(0.0f);
        m_fallDistance = 0.0f;
        // Open a brief grace window so the next few client-predicted
        // move packets (which may already be in flight at the new
        // position) don't trip the anti-cheat distance gate.
        m_teleportGraceUntil = std::chrono::steady_clock::now() +
                               std::chrono::seconds(2);
    }

    void ServerPlayer::snapTo(const glm::dvec3& pos) {
        m_position     = pos;
        m_movePacketBase = pos;
        m_velocity     = glm::vec3(0.0f);
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
        // "moved too quickly" test sits behind the player_movement_check rule
        // (shouldCheckPlayerMovement), and while gliding also behind
        // elytra_movement_check.
        if (distance > 600.0 && !inTeleportGrace &&
            Game::Rules::GetBool(Game::Rules::Id::PlayerMovementCheck) &&
            (!m_fallFlying || Game::Rules::GetBool(Game::Rules::Id::ElytraMovementCheck)) &&
            m_gameMode != GameMode::CREATIVE &&
            m_gameMode != GameMode::SPECTATOR) {
            Log::Warning("ServerPlayer: Player %u moved too fast (%.1f blocks)", m_playerId, distance);
            // TODO: Send position correction to client
            return;
        }

        m_position = pos;
        m_movePacketBase = pos;
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

    void ServerPlayer::damage(float amount, DamageSource source, const std::string& attackerName,
                              const Game::DamageSourceInfo& info) {
        const Game::DamageSourceInfo* previous = m_hurtSourceInfo;
        m_hurtSourceInfo = &info;
        damage(amount, source, attackerName);
        m_hurtSourceInfo = previous;
    }

    const char* ServerPlayer::damageTypeId(DamageSource source) {
        // MC DamageTypes keys. FIRE is the on-fire tick (the common case —
        // see bypassesArmor); ENTITY_ATTACK is a mob's melee.
        switch (source) {
            case DamageSource::GENERIC:            return "minecraft:generic";
            case DamageSource::GENERIC_KILL:       return "minecraft:generic_kill";
            case DamageSource::FALL:               return "minecraft:fall";
            case DamageSource::FIRE:               return "minecraft:on_fire";
            case DamageSource::LAVA:               return "minecraft:lava";
            case DamageSource::DROWNING:           return "minecraft:drown";
            case DamageSource::STARVATION:         return "minecraft:starve";
            case DamageSource::VOID_DAMAGE:        return "minecraft:out_of_world";
            case DamageSource::EXPLOSION:          return "minecraft:explosion";
            case DamageSource::ENTITY_ATTACK:      return "minecraft:mob_attack";
            case DamageSource::MAGIC:              return "minecraft:magic";
            case DamageSource::FALLING_BLOCK:      return "minecraft:falling_block";
            case DamageSource::FALLING_ANVIL:      return "minecraft:falling_anvil";
            case DamageSource::FALLING_STALACTITE: return "minecraft:falling_stalactite";
            case DamageSource::STALAGMITE:         return "minecraft:stalagmite";
            case DamageSource::WITHER:             return "minecraft:wither";
            case DamageSource::THORNS:             return "minecraft:thorns";
            case DamageSource::MACE_SMASH:         return "minecraft:mace_smash";
            case DamageSource::FIREWORKS:          return "minecraft:fireworks";
            case DamageSource::FLY_INTO_WALL:      return "minecraft:fly_into_wall";
            case DamageSource::LIGHTNING_BOLT:     return "minecraft:lightning_bolt";
            case DamageSource::SPEAR:              return "minecraft:spear";
            case DamageSource::TRIDENT:            return "minecraft:trident";
            case DamageSource::CACTUS:             return "minecraft:cactus";
            case DamageSource::SWEET_BERRY_BUSH:   return "minecraft:sweet_berry_bush";
            case DamageSource::HOT_FLOOR:          return "minecraft:hot_floor";
            case DamageSource::CAMPFIRE:           return "minecraft:campfire";
        }
        return "minecraft:generic";
    }

    void ServerPlayer::igniteForTicks(int ticks) {
        // MC LivingEntity.igniteForTicks: ceil(ticks * BURNING_TIME), then
        // Entity.igniteForTicks — the clock only grows.
        double burningTime = 1.0;
        if (Game::LivingEntity* view = effectEntity()) {
            burningTime = view->GetAttributeValue(Game::Attribute::BurningTime);
        }
        const int scaled = static_cast<int>(std::ceil(static_cast<double>(ticks) * burningTime));
        if (m_remainingFireTicks < scaled) m_remainingFireTicks = scaled;
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
        
        // MC Player.isInvulnerableTo: `abilities.invulnerable &&
        // !source.is(DamageTypeTags.BYPASSES_INVULNERABILITY)` — creative and
        // spectator shrug off everything but the tag's members, of which this
        // port has the void (fell_out_of_world) and /kill (generic_kill, which
        // goes through kill()). A spectator or creative player who flies 64
        // blocks under the world floor dies, as in vanilla.
        if ((m_gameMode == GameMode::CREATIVE || m_gameMode == GameMode::SPECTATOR) &&
            source != DamageSource::VOID_DAMAGE && source != DamageSource::GENERIC_KILL) {
            return;
        }

        // MC LivingEntity.hurtServer: FIRE_RESISTANCE refuses every #is_fire
        // source outright (on_fire, in_fire, lava, hot_floor and campfire) —
        // no hurt, no cooldown.
        if (isFireSource(source) && hasEffect(Game::MobEffectId::FireResistance)) {
            return;
        }

        // MC Player.isInvulnerableTo: the per-source game rules — a player
        // (only a player) takes no fall / fire / drowning damage while the
        // matching rule is off. Freeze damage has no source here (no powder
        // snow).
        {
            using Game::Rules::Id;
            // #is_fall: fall and stalagmite (an ender pearl's is FALL here).
            if ((source == DamageSource::FALL || source == DamageSource::STALAGMITE)
                                                 && !Game::Rules::GetBool(Id::FallDamage))     return;
            // #is_fire: on_fire, in_fire, lava, hot_floor and campfire all
            // sit behind fire_damage.
            if (isFireSource(source)             && !Game::Rules::GetBool(Id::FireDamage))     return;
            if (source == DamageSource::DROWNING && !Game::Rules::GetBool(Id::DrowningDamage)) return;
        }

        // The hit as MC's DamageSource: the caller's when it had one (the
        // view's hits carry the causing and direct entities), else the type
        // alone. The enchantment effects below all read this.
        Game::DamageSourceInfo info;
        if (m_hurtSourceInfo) {
            info = *m_hurtSourceInfo;
        } else {
            info.type = damageTypeId(source);
        }
        Game::LivingEntity* view = effectEntity();
        Game::EntityLevel* level = view ? view->Level() : nullptr;
        const Game::EnchantmentEquipment equipment = view ? Game::EnchantmentEquipment::Of(*view)
                                                          : Game::EnchantmentEquipment{};

        // MC LivingEntity.isInvulnerableTo → EnchantmentHelper
        // .isImmuneToDamage: a worn enchantment's damage_immunity refuses
        // the hit outright (Frost Walker boots against #burn_from_stepping).
        if (view && level && Game::EnchantmentHelper::IsImmuneToDamage(*level, *view, info, equipment)) {
            return;
        }
        
        // MC LivingEntity.hurtServer → applyItemBlocking: a raised
        // BLOCKS_ATTACKS item (past its block delay) absorbs what its damage
        // reductions cover for a hit from in front — BlocksAttacks.
        // resolveBlockedDamage over the angle between the facing and the
        // source — unless the type is in bypassed_by or a piercing arrow
        // made it; the item wears (hurtBlockingItem), a melee attacker is
        // pushed (blockUsingItem) and an axe-like weapon disables the block.
        // ENTITY_HURT_PLAYER's dealt (MC originalDamage) and blocked.
        const float originalAmount = amount;
        bool blockedAny = false;
        if (amount > 0.0f && isBlocking()) {
            const std::optional<Game::BlocksAttacks> blocks = m_useItem.get(Game::DataComponents::BLOCKS_ATTACKS);
            const auto* arrow = dynamic_cast<const Game::Arrow*>(info.direct);
            if (blocks && !Game::BlocksAttacksBypassedBy(*blocks, info.type) &&
                !(arrow && arrow->GetPierceLevel() > 0)) {
                double angle = 3.1415927410125732;
                const Game::Entity* sourceEntity = info.direct ? info.direct : info.causing;
                if (sourceEntity) {
                    const float yaw = getYaw() * 0.017453292f;
                    const glm::dvec3 view(-std::sin(yaw), 0.0, std::cos(yaw));
                    glm::dvec3 to = sourceEntity->position - m_position;
                    to.y = 0.0;
                    const double len = glm::length(to);
                    to = len > 1.0e-4 ? to / len : glm::dvec3(0.0);
                    angle = std::acos(std::clamp(glm::dot(to, view), -1.0, 1.0));
                }
                const float blocked = Game::ResolveBlockedDamage(*blocks, info.type, amount, angle);
                if (blocked > 0.0f) {
                    blockedAny = true;
                    // hurtBlockingItem: the item in the using hand wears.
                    const int wear = Game::BlockingItemDamage(*blocks, blocked);
                    const int slot = handSlotIndex(m_usedItemHand);
                    if (wear > 0) {
                        Game::ItemStack& using_ = m_inventory.MutableSlot(slot);
                        const Game::EquipmentSlot equipSlot = m_usedItemHand == 0 ? Game::EquipmentSlot::MAINHAND
                                                                                  : Game::EquipmentSlot::OFFHAND;
                        Game::HurtAndBreak(using_, wear, soundRandom(), isCreative(),
                                           [this, equipSlot](const Game::ItemStack& broken) {
                                               OnEquippedItemBroken(broken, equipSlot);
                                           });
                        markSlotDirty(slot);
                        if (using_.IsEmpty()) stopUsingItem();
                    }
                    // BlocksAttacks.onBlocked: the block sound.
                    if (Game::World* w = soundWorld(); w && !blocks->blockSound.empty()) {
                        w->PlaySound(nullptr, m_position, blocks->blockSound, Game::SoundSource::Players, 1.0f,
                                     0.8f + soundRandom().NextFloat() * 0.4f);
                    }
                    // blockUsingItem(attacker): a non-projectile hit from a
                    // living attacker — the push, then Player.blockUsingItem's
                    // disable by the attacker's WEAPON.
                    auto* attacker = dynamic_cast<Game::LivingEntity*>(info.direct);
                    if (attacker && !info.Is("#minecraft:is_projectile") && view) {
                        view->Knockback(0.5, view->position.x - attacker->position.x,
                                        view->position.z - attacker->position.z);
                        const Game::ItemStack* weapon = attacker->EquipmentInSlot(Game::EquipmentSlot::MAINHAND);
                        const auto w = weapon ? weapon->get(Game::DataComponents::WEAPON) : std::nullopt;
                        const float seconds = w ? w->disableBlockingForSeconds : 0.0f;
                        if (seconds > 0.0f) {
                            const int ticks = Game::DisableBlockingForTicks(*blocks, seconds);
                            if (ticks > 0) {
                                m_itemCooldowns.AddCooldown(m_useItem, ticks);
                                stopUsingItem();
                                if (Game::World* sw = soundWorld(); sw && !blocks->disableSound.empty()) {
                                    sw->PlaySound(nullptr, m_position, blocks->disableSound, Game::SoundSource::Players,
                                                  0.8f, 0.8f + soundRandom().NextFloat() * 0.4f);
                                }
                            }
                        }
                    }
                    amount -= blocked;
                    // Fully blocked: no hurt, no knockback, no damage event —
                    // but the block itself is ENTITY_HURT_PLAYER (the
                    // shield-a-projectile advancement).
                    if (amount <= 0.0f) {
                        CriteriaTriggers::EntityHurtPlayer(*this, info, originalAmount, 0.0f, true);
                        return;
                    }
                }
            }
        }

        // MC hurtServer's `damage` after the shield, before armour — what
        // ENTITY_HURT_PLAYER reports as taken.
        const float takenAmount = amount;

        // MC LivingEntity.hurtServer: a #damages_helmet source (a falling
        // block, anvil or stalactite) wears the helmet and loses a quarter
        // when one is worn.
        if ((source == DamageSource::FALLING_BLOCK || source == DamageSource::FALLING_ANVIL ||
             source == DamageSource::FALLING_STALACTITE) &&
            !m_inventory.GetSlot(Game::InventoryIndexFor(Game::EquipmentSlot::HEAD)).IsEmpty()) {
            doHurtEquipment(info, amount, {Game::EquipmentSlot::HEAD});
            amount *= 0.75f;
        }

        // MC LivingEntity.actuallyHurt → getDamageAfterArmorAbsorb: worn
        // armor takes its share of anything not in BYPASSES_ARMOR
        // (CombatRules.getDamageAfterAbsorb — toughness widens the band,
        // and armor never blocks more than 80% or 20 points). Player.hurtArmor
        // wears the four pieces first, on the damage before absorption.
        if (!bypassesArmor(source)) {
            doHurtEquipment(info, amount, {Game::EquipmentSlot::FEET, Game::EquipmentSlot::LEGS,
                                             Game::EquipmentSlot::CHEST, Game::EquipmentSlot::HEAD});
            const float armor = getArmorValue();
            if (armor > 0.0f) {
                const float toughness = getArmorToughness();
                const float f = 2.0f + toughness / 4.0f;
                const float g = std::clamp(armor - amount / f, armor * 0.2f, 20.0f);
                float armorFraction = g / 25.0f;
                // CombatRules.getDamageAfterAbsorb: the source's weapon
                // rewrites the armor fraction through its armor_effectiveness
                // (Breach), clamped to [0, 1].
                if (Game::ItemStack* weapon = info.GetWeaponItem(); weapon && view && level) {
                    armorFraction = std::clamp(Game::EnchantmentHelper::ModifyArmorEffectiveness(
                                                   *level, *weapon, *view, info, armorFraction),
                                               0.0f, 1.0f);
                }
                amount *= (1.0f - armorFraction);
            }
        }

        // MC getDamageAfterMagicAbsorb: nothing at all for #bypasses_effects
        // (starvation); RESISTANCE takes 5/25 per level off everything but
        // #bypasses_resistance (the void, /kill); then, unless
        // #bypasses_enchantments (the warden's sonic boom), the worn
        // enchantments' damage_protection — the Protection family and
        // Feather Falling — capped at 20 (CombatRules.getDamageAfterMagicAbsorb:
        // at most 80 % off).
        if (!info.Is("minecraft:bypasses_effects")) {
            if (!info.Is("minecraft:bypasses_resistance")) {
                if (const Game::MobEffectInstance* res = getEffect(Game::MobEffectId::Resistance)) {
                    const int absorbValue = (res->amplifier + 1) * 5;
                    amount = std::max(amount * static_cast<float>(25 - absorbValue) / 25.0f, 0.0f);
                }
            }
            if (amount <= 0.0f) {
                amount = 0.0f;
            } else if (!info.Is("minecraft:bypasses_enchantments") && view && level) {
                const float protection =
                    Game::EnchantmentHelper::GetDamageProtection(*level, *view, info, equipment);
                if (protection > 0.0f) {
                    amount *= 1.0f - std::clamp(protection, 0.0f, 20.0f) / 25.0f;
                }
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
        // MC Player.actuallyHurt: `if (dmg != 0) … gameEvent(ENTITY_DAMAGE)`
        // — a hurt player is heard by sculk sensors and wardens.
        if (amount != 0.0f) {
            if (Game::Entity* self = GameEventSource()) self->GameEvent(Game::GameEventId::EntityDamage);
        }
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
        // fire, DROWNING → drown, THORNS → the thorns prick, else the plain
        // hurt). Volume 1, the voice
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

        // MC hurtServer: CriteriaTriggers.ENTITY_HURT_PLAYER for every hit
        // that landed.
        CriteriaTriggers::EntityHurtPlayer(*this, info, originalAmount, takenAmount, blockedAny);

        // MC LivingEntity.hurtServer: `if (isDeadOrDying()) { if
        // (!checkTotemDeathProtection(source)) die(source); }`.
        if (m_health <= 0.0f && checkTotemDeathProtection(source)) {
            return;
        }
        if (m_health <= 0.0f) {
            playVoice(Game::SoundEvents::PLAYER_DEATH);
        } else {
            const char* hurt = Game::SoundEvents::PLAYER_HURT;
            // Player.getHurtSound: the damage type's `effects` — burning
            // (every #is_fire type here), drowning, thorns, poking (the
            // sweet berry bush).
            if (isFireSource(source))                                         hurt = Game::SoundEvents::PLAYER_HURT_ON_FIRE;
            else if (source == DamageSource::DROWNING)                        hurt = Game::SoundEvents::PLAYER_HURT_DROWN;
            else if (source == DamageSource::SWEET_BERRY_BUSH)                hurt = Game::SoundEvents::PLAYER_HURT_SWEET_BERRY_BUSH;
            else if (source == DamageSource::THORNS)                          hurt = Game::SoundEvents::THORNS_HIT;
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
            AwardDeathCriteria(*this, info);
        }
    }

    void ServerPlayer::setEnchantmentSeed(int seed) {
        // Player.readAdditionalSaveData: `if (enchantmentSeed == 0)
        // enchantmentSeed = random.nextInt()`.
        m_enchantmentSeed = seed != 0 ? seed : m_soundRandom.Next(32);   // random.nextInt()
    }

    void ServerPlayer::onEnchantmentPerformed(int enchantmentCost) {
        // MC Player.onEnchantmentPerformed: experienceLevel -= cost; going
        // below 0 clears the level, the bar and the total.
        const int level = m_experience.Level() - enchantmentCost;
        if (level < 0) {
            m_experience.SetLevel(0);
            m_experience.SetProgress(0.0f);
            m_experience.SetTotal(0);
        } else {
            m_experience.SetLevel(level);
        }
        m_enchantmentSeed = m_soundRandom.Next(32);   // random.nextInt()
    }

    void ServerPlayer::OnEquippedItemBroken(const Game::ItemStack& broken, Game::EquipmentSlot slot) {
        // MC LivingEntity.onEquippedItemBroken broadcasts the slot's entity
        // event and every client's breakItem plays the stack's BREAK_SOUND
        // locally (volume 0.8, pitch 0.8 + rand * 0.4, the entity's sound
        // source) and throws five ITEM particles. A player's view is not a
        // tracked entity here, so the event has no route; the sound goes out
        // from the server instead, to everyone the player included, which
        // is what every client would have played.
        (void)slot;
        if (broken.IsEmpty()) return;
        if (Game::World* w = soundWorld()) {
            const float pitch = 0.8f + m_soundRandom.NextFloat() * 0.4f;
            w->PlaySound(nullptr, m_position, Game::GetBreakSound(broken), Game::SoundSource::Players,
                         0.8f, pitch);
        }
        // breakItem's spawnItemParticles(stack, 5): the shards out of the
        // mouth along the look. Sent as five direct particles (count 0 —
        // the offsets are the velocity), everyone the player included.
        if (Game::World* w = soundWorld()) {
            const float eye = (m_sneaking && !isMorphed()) ? 1.27f * m_scale : getEyeHeight();
            const glm::dvec3 eyePos = m_position + glm::dvec3(0.0, eye, 0.0);
            const double xa = -static_cast<double>(getPitch()) * 0.017453292;
            const double ya = -static_cast<double>(getYaw()) * 0.017453292;
            const double xc = std::cos(xa), xs = std::sin(xa), yc = std::cos(ya), ys = std::sin(ya);
            const auto rotate = [&](glm::dvec3 v) {
                v = glm::dvec3(v.x, v.y * xc + v.z * xs, v.z * xc - v.y * xs);
                return glm::dvec3(v.x * yc + v.z * ys, v.y, v.z * yc - v.x * ys);
            };
            const Game::ParticleOptions options = Game::ParticleOptions::Item(broken.itemId);
            for (int i = 0; i < 5; ++i) {
                const glm::dvec3 d = rotate(glm::dvec3((m_soundRandom.NextFloat() - 0.5) * 0.1,
                                                       m_soundRandom.NextFloat() * 0.1 + 0.1, 0.0));
                const double y1 = static_cast<double>(-m_soundRandom.NextFloat()) * 0.6 - 0.3;
                const glm::dvec3 p = rotate(glm::dvec3((m_soundRandom.NextFloat() - 0.5) * 0.3, y1, 0.6)) + eyePos;
                w->SendParticles(options, false, false, p.x, p.y, p.z, 0, d.x, d.y + 0.05, d.z, 1.0);
            }
        }
        // stopLocationBasedEffects: armour and attack attributes are read
        // from the slots on demand, so the empty slot already stops them;
        // the enchantments' modifiers and location effects come off at the
        // view's next equipment diff (PlayerEntityView::TickEnchantments).
    }

    void ServerPlayer::doHurtEquipment(const Game::DamageSourceInfo& source, float damage,
                                       std::initializer_list<Game::EquipmentSlot> slots) {
        // MC LivingEntity.doHurtEquipment.
        if (damage <= 0.0f) return;
        const int durabilityDamage = static_cast<int>(std::max(1.0f, damage / 4.0f));
        for (const Game::EquipmentSlot slot : slots) {
            const int index = Game::InventoryIndexFor(slot);
            if (index < 0) continue;
            Game::ItemStack& stack = m_inventory.MutableSlot(index);
            const auto equippable = stack.get(Game::DataComponents::EQUIPPABLE);
            if (!equippable || !equippable->damageOnHurt || !Game::IsDamageableItem(stack)) continue;
            // ItemStack.canBeHurtBy: a DAMAGE_RESISTANT piece (netherite,
            // #minecraft:is_fire) is untouched by the sources it resists.
            if (const auto resistant = stack.get(Game::DataComponents::DAMAGE_RESISTANT);
                resistant && source.Is(*resistant)) continue;
            // itemStack.hurtAndBreak(durabilityDamage, this, slot): this is
            // the server's player, so hasInfiniteMaterials is creative (which
            // damage() has already turned away) and a break is ours to show.
            Game::HurtAndBreak(stack, durabilityDamage, m_soundRandom, isCreative(),
                               [this, slot](const Game::ItemStack& broken) {
                                   OnEquippedItemBroken(broken, slot);
                               });
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
        std::optional<Game::DeathProtection> protection;
        Game::ItemStack protectionItem;
        for (const int slot : hands) {
            Game::ItemStack& stack = m_inventory.MutableSlot(slot);
            if (stack.IsEmpty()) continue;
            protection = stack.get(Game::DataComponents::DEATH_PROTECTION);
            if (!protection) continue;
            protectionItem = stack;
            protectionItem.count = 1;
            // protectionItem = itemStack.copy(), then shrink(1); the copy's
            // causeUseVibration(this, ITEM_INTERACT_FINISH) follows below.
            CauseUseVibration(stack, Game::GameEventId::ItemInteractFinish);
            if (--stack.count <= 0) stack.Clear();   // itemStack.shrink(1)
            markSlotDirty(slot);
            break;
        }
        if (!protection) return false;

        // setHealth(1.0F), then the item's DeathProtection.applyEffects —
        // the totem's TOTEM_OF_UNDYING clears every effect and gives
        // REGENERATION II 45 s, ABSORPTION II 5 s, FIRE_RESISTANCE 40 s; a
        // /give'n death_protection runs its own list. (Statistics have no
        // system here.)
        m_health = 1.0f;
        // CriteriaTriggers.USED_TOTEM with the spent item.
        CriteriaTriggers::UsedTotem(*this, protectionItem);
        if (Game::LivingEntity* view = effectEntity()) {
            ServerPlayer* self = this;
            const auto move = [self](const glm::dvec3& to) { self->teleport(to); };
            for (const Game::ConsumeEffect& e : protection->deathEffects) {
                Game::ConsumableBehavior::ApplyConsumeEffect(*view, e, move);
            }
        }
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
        // The generic_kill blow has no entity: the credit is the last mob
        // to hurt the player, if any (MC getKillCredit).
        Game::DamageSourceInfo info;
        info.type = damageTypeId(DamageSource::GENERIC_KILL);
        AwardDeathCriteria(*this, info);
    }

    bool ServerPlayer::bypassesArmor(DamageSource source) {
        // MC DamageTypeTags.BYPASSES_ARMOR: on_fire, drown, starve, fall,
        // generic, generic_kill, magic, stalagmite, out_of_world … Armor
        // stands against attacks, projectiles, explosions, lava and things
        // falling on you. This port's FIRE is both in_fire and the on_fire
        // tick; the tick is the common case, so FIRE bypasses.
        switch (source) {
            case DamageSource::ENTITY_ATTACK:
            case DamageSource::MACE_SMASH:
            case DamageSource::SPEAR:
            case DamageSource::TRIDENT:
            case DamageSource::EXPLOSION:
            case DamageSource::FIREWORKS:   // not in #bypasses_armor
            case DamageSource::LIGHTNING_BOLT:   // not in #bypasses_armor
            case DamageSource::LAVA:
            case DamageSource::FALLING_BLOCK:
            case DamageSource::FALLING_ANVIL:
            case DamageSource::FALLING_STALACTITE:
            // The contact-damage blocks: none is in #bypasses_armor.
            case DamageSource::CACTUS:
            case DamageSource::SWEET_BERRY_BUSH:
            case DamageSource::HOT_FLOOR:
            case DamageSource::CAMPFIRE:
                return false;
            default:
                return true;
        }
    }

    bool ServerPlayer::isFireSource(DamageSource source) {
        // DamageTypeTags.IS_FIRE over this port's player sources.
        return source == DamageSource::FIRE || source == DamageSource::LAVA ||
               source == DamageSource::HOT_FLOOR || source == DamageSource::CAMPFIRE;
    }

    float ServerPlayer::getArmorValue() const {
        // MC LivingEntity.getArmorValue: Mth.floor(getAttributeValue(ARMOR)) —
        // every worn piece's ARMOR modifiers (any slot group, so a /give'n
        // sword with armour in "mainhand" counts too), clamped to 0..30.
        return static_cast<float>(std::floor(getAttributeValue(Game::Attribute::Armor)));
    }

    float ServerPlayer::getArmorToughness() const {
        // MC getAttributeValue(ARMOR_TOUGHNESS) (CombatRules' toughness).
        return static_cast<float>(getAttributeValue(Game::Attribute::ArmorToughness));
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

    // ── Player attributes (MC Player.createAttributes, the worn items'
    //    ATTRIBUTE_MODIFIERS and enchantment modifiers, the effect
    //    templates — the fold the client's ClientPlayer runs too) ────────

    double ServerPlayer::getAttributeValue(Game::Attribute attribute) const {
        return attributeInstance(attribute).GetValue();
    }

    Game::AttributeInstance ServerPlayer::attributeInstance(Game::Attribute attribute) const {
        Game::AttributeInstance instance = Game::EnchantmentHelper::PlayerAttributeInstance(
            attribute, &m_attributes, m_inventory, m_activeEffects);
        // MC ServerPlayer.updatePlayerAttributes: in creative the two reach
        // attributes carry CREATIVE_BLOCK_INTERACTION_RANGE_MODIFIER (+0.5)
        // and CREATIVE_ENTITY_INTERACTION_RANGE_MODIFIER (+2.0), transient
        // ADD_VALUE. Folded here rather than stored, so a game-mode change
        // needs no bookkeeping (the client folds the same pair itself).
        if (m_gameMode == GameMode::CREATIVE) {
            if (attribute == Game::Attribute::BlockInteractionRange) {
                static const uint32_t kId = static_cast<uint32_t>(
                    Game::NamedModifierId(Game::kCreativeBlockRangeModifierName));
                instance.AddModifier(Game::AttributeModifier{
                    kId,
                    Game::kCreativeBlockInteractionRangeBonus, Game::AttributeOperation::AddValue});
            } else if (attribute == Game::Attribute::EntityInteractionRange) {
                static const uint32_t kId = static_cast<uint32_t>(
                    Game::NamedModifierId(Game::kCreativeEntityRangeModifierName));
                instance.AddModifier(Game::AttributeModifier{
                    kId,
                    Game::kCreativeEntityInteractionRangeBonus, Game::AttributeOperation::AddValue});
            }
        }
        return instance;
    }

    void ServerPlayer::applyStepHeightRule(int tenths) {
        // An ADD_VALUE that lifts STEP_HEIGHT's 0.6 to the rule's value —
        // absent at the default, so a vanilla world's players carry nothing.
        static const Game::ModifierId id = Game::NamedModifierId(Game::kStepHeightRuleModifierName);
        const double amount = static_cast<double>(std::clamp(tenths, 0, 100)) / 10.0 -
                              Game::PlayerBaseAttributeValue(Game::Attribute::StepHeight);
        const Game::AttributeInstance* row = m_attributes.Find(Game::Attribute::StepHeight);
        const Game::AttributeModifier* current = row ? row->FindModifier(static_cast<uint32_t>(id)) : nullptr;
        if (std::abs(amount) < 1.0e-9) {
            if (current) m_attributes.RemoveModifier(Game::Attribute::StepHeight, id);
            return;
        }
        if (current && current->amount == amount) return;
        m_attributes.AddModifier(Game::Attribute::StepHeight, Game::AttributeModifier{
            static_cast<uint32_t>(id), amount, Game::AttributeOperation::AddValue});
    }

    float ServerPlayer::getMaxHealth() const {
        return static_cast<float>(getAttributeValue(Game::Attribute::MaxHealth));
    }

    float ServerPlayer::getMaxAbsorption() const {
        return static_cast<float>(getAttributeValue(Game::Attribute::MaxAbsorption));
    }

    float ServerPlayer::getFallDamageMultiplier() const {
        return static_cast<float>(getAttributeValue(Game::Attribute::FallDamageMultiplier));
    }

    float ServerPlayer::getReachDistance() const {
        // MC Player.blockInteractionRange: BLOCK_INTERACTION_RANGE, with
        // ServerPlayer's CREATIVE_BLOCK_INTERACTION_RANGE_MODIFIER (+0.5
        // ADD_VALUE — attributeInstance folds it in).
        return static_cast<float>(getAttributeValue(Game::Attribute::BlockInteractionRange));
    }

    double ServerPlayer::entityInteractionRange() const {
        // MC Player.entityInteractionRange with the creative +2.0.
        return getAttributeValue(Game::Attribute::EntityInteractionRange);
    }

    void ServerPlayer::clampToEffectMaxima() {
        const float maxHealth = getMaxHealth();
        if (m_health > maxHealth) m_health = maxHealth;
        const float maxAbsorption = getMaxAbsorption();
        if (m_absorptionAmount > maxAbsorption) m_absorptionAmount = maxAbsorption;
    }

    float ServerPlayer::getSafeFallDistance() const {
        return static_cast<float>(getAttributeValue(Game::Attribute::SafeFallDistance));
    }

    float ServerPlayer::getLuck() const {
        return static_cast<float>(getAttributeValue(Game::Attribute::Luck));
    }

    float ServerPlayer::getAttackDamage() const {
        return static_cast<float>(getAttributeValue(Game::Attribute::AttackDamage));
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

    void ServerPlayer::syncAttributesToClient(bool force) {
        // MC ServerEntity.sendDirtyEntityData → ClientboundUpdateAttributes
        // Packet(getId(), dirty syncable attributes) — the owner included,
        // whose LocalPlayer folds them into its prediction (step height,
        // gravity, jump strength, reach, scale …). The whole syncable set
        // goes, as UpdateAttributesS2C always sends it.
        const uint64_t signature = Network::SyncableAttributesSignature(m_attributes);
        if (!force && m_attributesSent && signature == m_sentAttributesSignature) return;
        ServerConnection* connection = ConnectionFor(m_playerId);
        if (!connection) return;
        Network::UpdateAttributesS2CPacket packet;
        packet.entityId = static_cast<int32_t>(m_playerId);
        packet.attributes = Network::SyncableAttributes(m_attributes);
        connection->SendPacket(static_cast<uint8_t>(Network::PacketId::UpdateAttributesS2C),
                               Network::Serialization::Serialize(packet));
        m_sentAttributesSignature = signature;
        m_attributesSent = true;
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

    // MC ServerItemCooldowns.onCooldownStarted / onCooldownEnded:
    // ClientboundCooldownPacket(group, duration) — 0 for an ended one.
    void ServerItemCooldowns::OnCooldownStarted(const std::string& group, int duration) {
        Game::ItemCooldowns::OnCooldownStarted(group, duration);
        ServerConnection* connection = ConnectionFor(m_owner.getPlayerId());
        if (!connection) return;
        Network::CooldownS2CPacket p;
        p.cooldownGroup = group;
        p.duration      = duration;
        connection->SendPacket(static_cast<uint8_t>(Network::PacketId::CooldownS2C),
                               Network::Serialization::Serialize(p));
    }

    void ServerItemCooldowns::OnCooldownEnded(const std::string& group) {
        Game::ItemCooldowns::OnCooldownEnded(group);
        ServerConnection* connection = ConnectionFor(m_owner.getPlayerId());
        if (!connection) return;
        Network::CooldownS2CPacket p;
        p.cooldownGroup = group;
        p.duration      = 0;
        connection->SendPacket(static_cast<uint8_t>(Network::PacketId::CooldownS2C),
                               Network::Serialization::Serialize(p));
    }

    // === ABILITIES ===

    bool ServerPlayer::setGameMode(GameMode mode) {
        // MC changeGameModeForPlayer: `if (gameModeForPlayer == this.gameModeForPlayer) return false`.
        if (mode == m_gameMode) return false;
        // setGameModeForPlayer(gameModeForPlayer, previousGameModeForPlayer = the old one).
        m_previousGameMode = static_cast<int>(m_gameMode);
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

        // MC ServerPlayerGameMode.changeGameModeForPlayer: going creative
        // drops any running impulse (resetCurrentImpulseContext).
        if (mode == GameMode::CREATIVE) m_impulseContext.Reset();

        Log::Info("ServerPlayer: Player %u game mode changed to %d", m_playerId, static_cast<int>(mode));
        return true;
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
        // A passenger's body is its seat's (Server::PlayerRiding puts it
        // there every tick): no gravity, no drift. Integrating here moved a
        // rider off the seat between two seatings, which read as a teleport
        // and ended the ride.
        if (isPassenger()) {
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