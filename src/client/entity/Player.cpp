// File: src/client/entity/Player.cpp
#include "Player.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/world/level/World.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/Log.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "RemotePlayerManager.hpp"
#include "ClientMobManager.hpp"
#include "../world/ClientLevel.hpp"
#include <algorithm>
#include <cmath>
#include "common/core/Mth.hpp"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include "common/core/Features.hpp"
#include "client/world/ClientLevel.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#endif

namespace Game {

    ClientPlayer::ClientPlayer() {
        Initialize();
    }

    void ClientPlayer::Initialize() {
        // Initialize inventory with default blocks
        inventory.InitializeDefaults();

        // Initialize physics at a safe spawn position
        physics.position = glm::vec3(0.0f, 67.0f, 0.0f);
        physics.velocity = glm::vec3(0.0f);
        physics.isOnGround = false;
        physics.isSneaking = false;
        physics.isSprinting = false;
        physics.noclip = false;

        // Initialize transform tracking
        serverPos = physics.position;
        predictedPos = glm::dvec3(physics.position);
        visualPos = glm::dvec3(physics.position);
        
        lastPosition = physics.position;

        Log::Info("ClientPlayer initialized at position (%.2f, %.2f, %.2f)",
                  physics.position.x, physics.position.y, physics.position.z);
    }

    void ClientPlayer::Tick() {
        // MC Player.tick:257-266. These are the client's own copies of the
        // attack cooldown — the attack indicator IS attackStrengthTicker, and
        // the server keeps an identical one so the bar and the damage agree.
        //
        // They live in the 20 Hz tick, NOT in UpdatePhysics: that runs once per
        // FRAME, so counting there filled the bar at frame rate (three times
        // too fast at 60 fps) and the indicator read full while the server was
        // still charging.
        ++attackStrengthTicker;
        ++itemSwapTicker;

        // MC compares the ITEM (ItemStack.isSameItem), not the slot: moving
        // between two hotbar slots holding the same sword keeps the charge,
        // and a stack that changes under a stationary selection loses it.
        //
        // An item change resets BOTH tickers (Player.resetAttackStrengthTicker);
        // an ATTACK resets only the attack one (onAttack →
        // resetOnlyAttackStrengthTicker, done in the controller's attack path).
        const uint32_t heldNow = static_cast<uint32_t>(inventory.GetSelectedItem());
        if (heldNow != m_lastItemInMainHand) {
            m_lastItemInMainHand = heldNow;
            attackStrengthTicker = 0;
            itemSwapTicker = 0;
        }

        // MC LivingEntity.baseTick counts the hurt flash down; tickDeath counts
        // the death animation up and stops at 20 (where the camera roll's
        // asymptote leaves it).
        if (hurtTime > 0) --hurtTime;
        if (damageCooldownTime > 0) --damageCooldownTime;
        if (health <= 0) {
            if (deathTime < 20) ++deathTime;
        } else {
            deathTime = 0;
        }

        // MC Player.tick:228-248 — the sleep clock. Clamped at 100 while in
        // bed (the fade-in is complete), and once up it keeps counting to
        // 110 before resetting (the fade-out). The server-side half of the
        // same block — waking when the night is over — is the server's.
        if (IsSleeping()) {
            if (++sleepCounter > 100) sleepCounter = 100;
        } else if (sleepCounter > 0) {
            if (++sleepCounter >= 110) sleepCounter = 0;
        }

        TickEffects();
    }

    // ── Status effects (MC LocalPlayer / LivingEntity, client side) ────────

    void ClientPlayer::TickEffects() {
        // MC LivingEntity.tickEffects, client branch: every instance counts
        // down and blends (the server's RemoveMobEffect confirms the expiry).
        ++tickCount;
        for (Game::MobEffectInstance& e : activeEffects) e.TickClient();

        // MC LocalPlayer.tickSpinningEffect, with no portal term (see the
        // header): the nausea warp turns at 7 degrees a tick, blended in.
        const float nauseaIntensity = GetEffectBlendFactor(Game::MobEffectId::Nausea, 1.0f);
        if (!(nauseaIntensity > 0.0f)) {
            spinningEffectSpeed = 0.0f;
        } else {
            spinningEffectSpeed = (nauseaIntensity * 7.0f) / nauseaIntensity;
            spinningEffectTime += spinningEffectSpeed;
        }
    }

    void ClientPlayer::ApplyEffectUpdate(Game::MobEffectInstance effect, bool blend) {
        if (!blend) effect.SkipBlending();
        for (Game::MobEffectInstance& e : activeEffects) {
            if (e.effect != effect.effect) continue;
            // forceAddEffect: newEffect.copyBlendState(previousEffect).
            effect.CopyBlendState(e);
            e = Game::MobEffectInstance(effect);
            e.CopyBlendState(effect);
            return;
        }
        activeEffects.push_back(std::move(effect));
    }

    void ClientPlayer::ApplyEffectRemove(Game::MobEffectId id) {
        activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(),
                                           [id](const Game::MobEffectInstance& e) { return e.effect == id; }),
                            activeEffects.end());
    }

    float ClientPlayer::GetMaxHealth() const {
        return static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::MaxHealth, 20.0, activeEffects));
    }

    double ClientPlayer::EnchantedAttributeValue(Game::Attribute attribute, double base) {
        return Game::EnchantmentHelper::PlayerAttributeValue(attribute, base, inventory, activeEffects,
                                                             &enchantmentLocationAttributes);
    }

    float ClientPlayer::GetMiningEfficiency() {
        return static_cast<float>(EnchantedAttributeValue(Game::Attribute::MiningEfficiency, 0.0));
    }

    float ClientPlayer::GetSubmergedMiningSpeed() {
        return static_cast<float>(EnchantedAttributeValue(Game::Attribute::SubmergedMiningSpeed, 0.2));
    }

    void ClientPlayer::UpdateEnchantmentLocationEffects(IBlockAccess* blockAccess) {
        // The facts the location conditions read (EntityPredicate on the
        // local player): where it stands, how it moves, whether it flies.
        Game::EnchantmentEntityFacts facts;
        facts.typeId        = "minecraft:player";
        facts.position      = physics.position;
        facts.knownMovement = glm::dvec3(physics.velocity) / 20.0;
        facts.tickCount     = tickCount;
        facts.onGround      = physics.isOnGround;
        facts.crouching     = physics.isSneaking;
        facts.sprinting     = physics.isSprinting;
        facts.flying        = physics.isFlying;
        facts.inWater       = physics.isInWater;

        Game::EnchantmentEquipment equipment = Game::EnchantmentEquipment::OfInventory(inventory);
        equipment.attributes = &enchantmentLocationAttributes;
        equipment.facts = &facts;

        // collectEquipmentChanges: a changed slot's location effects stop,
        // and the new item's run.
        static constexpr Game::EquipmentSlot kSlots[] = {
            Game::EquipmentSlot::MAINHAND, Game::EquipmentSlot::OFFHAND, Game::EquipmentSlot::FEET,
            Game::EquipmentSlot::LEGS, Game::EquipmentSlot::CHEST, Game::EquipmentSlot::HEAD,
        };
        for (const Game::EquipmentSlot slot : kSlots) {
            Game::ItemStack& last = enchantmentLastEquipment[static_cast<size_t>(slot)];
            const Game::ItemStack* now = equipment.itemBySlot(slot);
            const Game::ItemStack current = now ? *now : Game::ItemStack{};
            if (Game::ItemStacksMatch(current, last)) continue;
            Game::EnchantmentHelper::StopLocationBasedEffectsInSlot(equipment, enchantmentLocationState, slot);
            last = current;
            if (!current.IsEmpty() && !Game::IsBrokenItem(current)) {
                Game::EnchantmentHelper::RunLocationChangedEffectsInSlot(
                    nullptr, blockAccess, enchantmentRandom, equipment, enchantmentLocationState, slot);
            }
        }

        // onChangedBlock: a new block position re-evaluates everything worn.
        const glm::ivec3 blockPos(static_cast<int>(std::floor(physics.position.x)),
                                  static_cast<int>(std::floor(physics.position.y)),
                                  static_cast<int>(std::floor(physics.position.z)));
        if (!enchantmentHasLastBlockPos || blockPos != enchantmentLastBlockPos) {
            enchantmentLastBlockPos = blockPos;
            enchantmentHasLastBlockPos = true;
            Game::EnchantmentHelper::RunLocationChangedEffects(nullptr, blockAccess, enchantmentRandom,
                                                               equipment, enchantmentLocationState);
        }
    }

    void ClientPlayer::ApplyEffectPhysics() {
        // MOVEMENT_SPEED's factors over the walk — the attribute at the
        // player's base 0.1 with the effect templates and Soul Speed's
        // location bonus, relative to that base (the walk constants already
        // are the 0.1).
        physics.effectSpeedFactor = static_cast<float>(
            EnchantedAttributeValue(Game::Attribute::MovementSpeed, 0.1) / 0.1);
        physics.sneakingSpeed = static_cast<float>(
            EnchantedAttributeValue(Game::Attribute::SneakingSpeed, Game::PlayerPhysics::SNEAKING_SPEED));
        physics.waterMovementEfficiency = static_cast<float>(
            EnchantedAttributeValue(Game::Attribute::WaterMovementEfficiency, 0.0));
        physics.movementEfficiency = static_cast<float>(
            EnchantedAttributeValue(Game::Attribute::MovementEfficiency, 0.0));
        const Game::MobEffectInstance* jump = GetEffect(Game::MobEffectId::JumpBoost);
        physics.effectJumpBoost = jump ? 0.1f * static_cast<float>(jump->amplifier + 1) : 0.0f;
        const Game::MobEffectInstance* levitation = GetEffect(Game::MobEffectId::Levitation);
        physics.effectLevitation = levitation ? levitation->amplifier : -1;
        physics.effectSlowFalling   = HasEffect(Game::MobEffectId::SlowFalling);
        physics.effectDolphinsGrace = HasEffect(Game::MobEffectId::DolphinsGrace);
    }

    float ClientPlayer::GetCurrentItemAttackStrengthDelay() const {
        float itemDamage = 0.0f, itemSpeed = 0.0f;
        Game::GetItemAttackAttributes(
            static_cast<uint32_t>(inventory.GetSelectedItem()), itemDamage, itemSpeed);
        // ATTACK_SPEED with HASTE / MINING_FATIGUE — the same fold the
        // server's ServerPlayer does, so the bar and the damage agree.
        const float attackSpeed = static_cast<float>(Game::ComputeAttributeWithEffects(
            Game::Attribute::AttackSpeed,
            static_cast<double>(Game::kPlayerBaseAttackSpeed + itemSpeed), activeEffects));
        // A pathological modifier could zero this; a NaN delay would make every
        // swing read as fully charged. Mirrors ServerPlayer's guard.
        if (attackSpeed <= 0.0f) return 1.0e6f;
        return 20.0f / attackSpeed;
    }

    float ClientPlayer::GetAttackStrengthScale(float adjust) const {
        const float v = (static_cast<float>(attackStrengthTicker) + adjust)
                      / GetCurrentItemAttackStrengthDelay();
        return std::clamp(v, 0.0f, 1.0f);
    }

    float ClientPlayer::GetItemSwapScale(float adjust) const {
        const float v = (static_cast<float>(itemSwapTicker) + adjust)
                      / GetCurrentItemAttackStrengthDelay();
        return std::clamp(v, 0.0f, 1.0f);
    }

    void ClientPlayer::UpdatePhysics(float deltaTime, IBlockAccess* blockAccess) {
        // A flier's morph flies, always: the mob never touches down, so
        // neither does the body (the landing cancel in UpdatePlayerPhysics
        // is undone every frame, the double-tap toggle never wins).
        if (IsMorphed() && Game::Morph::IsFlier(morph)) {
            physics.isFlying = true;
        }
        if (IsMorphed() && Game::Morph::IsMob(morph, Game::EntityTypeId::Chicken) &&
            !physics.isOnGround && !physics.isFlying && physics.velocity.y < 0.0f) {
            // MC Chicken.aiStep: falling, the vertical motion is scaled by
            // 0.6 every tick — the flap-down.
            physics.velocity.y *= std::pow(0.6f, deltaTime * 20.0f);
        }
        if (heldByPlayer != 0 && Client::g_remotePlayerManager) {
            // /morph item, picked up: the body is wherever the holder is —
            // in their hand, as far as the world knows.
            const auto& players = Client::g_remotePlayerManager->GetPlayers();
            if (auto it = players.find(heldByPlayer); it != players.end() && it->second.positionInitialized) {
                // In the holder's RIGHT hand: off the body's yaw (not the
                // head's), a hand's width to the right and a little forward
                // of the hip, at hand height — where the third-person held
                // item sits. MC's facing convention: forward (−sin, 0, cos),
                // so the right side is (−cos, 0, −sin).
                const Client::RemotePlayer& holder = it->second;
                const float yaw = glm::radians(holder.bodyYaw);
                const glm::dvec3 forward(-std::sin(yaw), 0.0, std::cos(yaw));
                const glm::dvec3 right(-std::cos(yaw), 0.0, -std::sin(yaw));
                const double sc = holder.scale;
                physics.position = holder.position + right * (0.4 * sc) + forward * (0.25 * sc) +
                                   glm::dvec3(0.0, 0.85 * sc, 0.0);
            }
            physics.velocity    = glm::vec3(0.0f);
            physics.isOnGround  = true;
            physics.isSneaking  = false;
            physics.isSprinting = false;
            predictedPos = physics.position;
            jumpPressed  = false;
            return;
        }
        if (IsMorphed() && Game::Morph::KindOf(morph) == Game::Morph::Kind::Xp &&
            Client::g_remotePlayerManager) {
            // /morph xp: drawn to the nearest player like an orb (MC
            // ExperienceOrb.followNearbyPlayer: within 8 blocks, toward
            // half their eye height, (1 − d/8)² × 0.1 per tick) — and never
            // taken, so it keeps swirling about them.
            const Client::RemotePlayer* nearest = nullptr;
            double nearestSq = 64.0;
            for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (rp.invisible || !rp.positionInitialized) continue;
                if (!Client::IsRemotePlayerInBoundLevel(rp)) continue;
                const glm::dvec3 d = rp.position - physics.position;
                const double dSq = glm::dot(d, d);
                if (dSq < nearestSq) { nearestSq = dSq; nearest = &rp; }
            }
            if (nearest) {
                const glm::dvec3 delta = nearest->position + glm::dvec3(0.0, 1.62 * 0.5 * nearest->scale, 0.0)
                                         - physics.position;
                const double len = glm::length(delta);
                if (len > 1.0e-6) {
                    const double power = 1.0 - std::sqrt(nearestSq) / 8.0;
                    // 0.1 blocks per tick each tick → blocks per second, over this frame.
                    const double impulse = power * power * 0.1 * 20.0 * (deltaTime * 20.0);
                    physics.velocity += glm::vec3(delta / len * impulse);
                }
            }
        }
        if (morphGridLock) {
            // The real block stands in the cell while locked; once it has
            // been there and is gone, it was mined — release.
            if (blockAccess) {
                const auto block = static_cast<Game::BlockID>(Game::Morph::BlockOf(morph));
                const glm::ivec3 cell(static_cast<int>(std::floor(morphLockPos.x)),
                                      static_cast<int>(std::floor(morphLockPos.y + 0.5)),
                                      static_cast<int>(std::floor(morphLockPos.z)));
                const bool there = blockAccess->GetBlock(cell.x, cell.y, cell.z) == block;
                if (there) morphLockBlockSeen = true;
                else if (morphLockBlockSeen) {
                    morphGridLock = false;
                    morphLockBlockSeen = false;
                }
            }
        }
        if (IsMorphed() && Game::Morph::IsMob(morph, Game::EntityTypeId::Armadillo) &&
            Game::Morph::IsSheared(morph)) {
            // Rolled up (MC Armadillo.rollUp: stopInPlace, and the brain's
            // ball-up activity holds it): no walking, no jumping — gravity
            // and the ground still apply.
            movementInput = glm::vec3(0.0f);
            jumpPressed   = false;
            jumpHeld      = false;
            sprintPressed = false;
        }
        if (morphGridLock) {
            // Held on the grid: nothing moves it, not input, not gravity,
            // not a nudge — a placed block.
            physics.position    = morphLockPos;
            physics.velocity    = glm::vec3(0.0f);
            physics.isOnGround  = true;
            physics.isSneaking  = false;
            physics.isSprinting = false;
            // The render position follows predictedPos; without this the
            // body drew where it was before the snap until the release.
            predictedPos = morphLockPos;
            jumpPressed  = false;
            return;
        }
        // Update physics state based on input
        physics.isSneaking = sneakPressed;
        // Sneak normally blocks sprinting, but shift while flying is "descend",
        // not crouch — MC's canStartSprinting only rejects on isMovingSlowly()
        // → isCrouching(), and the crouch pose is never entered while
        // abilities.flying (LocalPlayer.java:1075, 622). Without the flying
        // exemption, sprint-descending would silently drop back to base speed.
        //
        // Noclip needs the SAME exemption and is a separate flag, not a kind of
        // isFlying. Missing it made sprint asymmetric in noclip: ascending
        // (jump) kept the boost because sneak was not held, descending (sneak)
        // silently lost it. Ascend and descend are the same gesture there, so
        // they must answer the same.
        physics.isSprinting =
            sprintPressed && (!sneakPressed || physics.isFlying || physics.noclip);
        // MC LocalPlayer.isSprintingPossible: isMobilityRestricted (BLINDNESS)
        // forbids sprinting outright — and stops a sprint already running.
        if (IsMobilityRestricted() && !physics.noclip) physics.isSprinting = false;
        // The local status effects the step reads (speed, jump boost,
        // levitation, slow falling, dolphin's grace), and the enchantment
        // attributes (Swift Sneak, Depth Strider, Soul Speed) after the
        // location effects have caught up with where the player stands.
        UpdateEnchantmentLocationEffects(blockAccess);
        ApplyEffectPhysics();

        // Create physics context with block access (World, ClientBlockAccess, etc.)
        PhysicsContext context;
        context.blockAccess = blockAccess;
        // MC Player.travel's swim steering reads getLookAngle().y.
        physics.lookDirY = lookDir.y;
        // MC EnvironmentAttributes.FAST_LAVA: the nether's lava pushes harder.
        context.fastLava = Client::ClientLevels::HasSession() &&
                           Client::ClientLevels::ActiveDimension() == Game::DimensionId::Nether;

        // In a fluid, use held state so holding space continuously bobs upward.
        // On land, use edge-triggered jumpPressed for single jumps.
        const bool inFluid = physics.isInWater || physics.isInLava;
        bool jumpInput = inFluid ? jumpHeld : jumpPressed;
        // A creeper morph's Shift+Space is its swell, not a jump.
        if (IsCreeperMorph() && sneakPressed) jumpInput = false;

        // Apply physics simulation with context.
        //
        // In steps of at most one tick. The frame delta is whatever the
        // clock says, and on macOS a window resize or a title-bar drag
        // blocks the event loop for as long as the mouse is down — the next
        // frame arrives with a delta of whole seconds. Integrated in one
        // go, gravity builds to a huge velocity and the position moves far
        // enough in a single step to land past the floor (the collision
        // snap only sees the start and end of the step), so a resize
        // dropped the player through the ground. A long pause is also
        // bounded to a quarter second of simulation: the world stood still
        // while the window was being dragged, so the player does too.
        constexpr float kMaxStep      = 0.05f;   // one tick
        constexpr float kMaxSimulated = 0.25f;
        float remaining = std::min(deltaTime, kMaxSimulated);
        bool  first = true;
        bool  jumped = false;   // the edge below reads it after the last step
        do {
            const float step = std::min(remaining, kMaxStep);
            // A jump is an edge, pressed once; only the first step sees it
            // (held-state water bobbing is repeated, like the key).
            const bool stepJump = jumpInput && (first || physics.isInWater || physics.isInLava);
            UpdatePlayerPhysics(physics, movementInput, stepJump, sneakPressed, step, context);
            jumped |= physics.didJumpThisStep;
            remaining -= step;
            first = false;
        } while (remaining > 1e-6f);
        physics.didJumpThisStep = jumped;

        // Accumulate jump impulses for the next PlayerMoveC2S (server-side
        // jump exhaustion — MC ServerPlayer.jumpFromGround). Cleared by the
        // move-send in PlatformMain each client tick.
        if (physics.didJumpThisStep) {
            jumpedSinceMoveSend = true;
        }

        // Flush this step's fall landing (if any) toward the next move
        // packet. Multiple landings inside one tick keep the largest.
        if (physics.landedFallDistance > 0.0f) {
            landedFallSinceMoveSend =
                std::max(landedFallSinceMoveSend, physics.landedFallDistance);
            landedFallForSound = std::max(landedFallForSound, physics.landedFallDistance);
            physics.landedFallDistance = 0.0f;
        }

        // Update predicted position from physics
        predictedPos = glm::dvec3(physics.position);

        // Reset single-frame inputs
        jumpPressed = false;

        // Double-tap window countdown (MC decrements jumpTriggerTime per tick)
        if (flyToggleTimer > 0.0f) {
            flyToggleTimer -= deltaTime;
            if (flyToggleTimer < 0.0f) flyToggleTimer = 0.0f;
        }

        // Update mesh system with player position
        if (::Render::g_clientMeshManager) {
            ::Render::g_clientMeshManager->SetPlayerPosition(physics.position);
        }
    }

    void ClientPlayer::UpdateRaycast(const Render::Camera& camera) {
        // Calculate ray direction from camera
        const glm::vec3 front = camera.GetForward();

        // Cache the live look direction so consumers (e.g. portal-gun
        // projectile spawn) can use the camera-space forward without
        // having to re-derive it from stale yaw/pitch fields.
        lookDir = front;

        // Cast ray from camera position (player's eyes)
        // MC BLOCK_INTERACTION_RANGE: 4.5, +0.5 in creative (ServerPlayer's
        // creative modifier), scaled with the body. The server allows one
        // block of slack on top of this for a break, never less — so a
        // block this ray reaches is one the server lets us break.
        const float kReach = (IsCreative() ? 5.0f : 4.5f) * physics.scale;
        lastBlockHit = Raycast::CastRay(camera.position, front, kReach);
        lastBlockHitDimension = Client::ClientLevels::HasSession()
            ? Client::ClientLevels::ActiveDimension() : Game::DimensionId::Overworld;
        lastBlockHitPortalId  = 0;

#if ENABLE_IMMERSIVE_PORTALS
        // Through a portal: if the ray pierces a see-through surface before
        // it hits anything, the rest of it continues in the portal's
        // destination level, from the mapped point along the mapped
        // direction. One portal deep — reaching through two at once is
        // not something a hand does.
        if (Client::ClientLevels::HasSession()) {
            namespace PortalFlag = Game::Immersive::PortalFlag;
            const glm::dvec3 from(camera.position);
            // The segment runs a centimetre PAST the block hit: a wall-mounted
            // surface sits a thousandth of a block off the face the ray
            // stopped on, and the hit point must count as behind it.
            const float reachToBlock = lastBlockHit ? lastBlockHit->distance + 0.01f : kReach;
            const glm::dvec3 to = from + glm::dvec3(front) * static_cast<double>(reachToBlock);
            const Game::Immersive::Portal* best = nullptr;
            double bestT = 2.0;
            glm::dvec3 pierce{0.0};
            Client::GetClientImmersivePortals().ForEach([&](const Game::Immersive::Portal& p) {
                if (!p.Has(PortalFlag::Interactable) || !p.Has(PortalFlag::Visible)) return;
                // Same tolerance as a crossing: a gun portal counts as its
                // whole 1×2 opening, not just the oval.
                const auto hit = p.RaytraceSegment(from, to, p.CrossingLeniency());
                if (!hit || hit->t >= bestT) return;
                best = &p; bestT = hit->t; pierce = hit->point;
            });
            if (best) {
                const Game::Immersive::Portal portal = *best;   // copy: the level rebinds below
                const double travelled = glm::length(pierce - from);
                const float  remaining = kReach - static_cast<float>(travelled);
                const glm::dvec3 farOrigin = portal.TransformPoint(pierce);
                const glm::dvec3 farDir    = glm::normalize(portal.TransformLocalVecNonScale(glm::dvec3(front)));
                const Game::DimensionId dest = portal.IsMirror() ? portal.dimension : portal.destDimension;
                std::optional<RaycastHit> farHit;
                if (remaining > 0.0f) {
                    // WithLevel binds the far level's block access for the
                    // duration — the same access the ray reads.
                    Client::ClientLevels::WithLevel(dest, [&]() {
                        farHit = Raycast::CastRay(farOrigin + farDir * 0.001,
                                                  glm::vec3(farDir), remaining);
                    });
                }
                if (farHit) farHit->distance += static_cast<float>(travelled);
                lastBlockHit          = farHit;   // nothing behind the surface counts
                lastBlockHitDimension = dest;
                lastBlockHitPortalId  = portal.id;
            }
        }
#endif
    }

    void ClientPlayer::UpdateVisual(float deltaTime) {
        // TODO: Implement smooth interpolation between server and predicted positions
        // For now, just copy predicted position directly (no smoothing)
        visualPos = predictedPos;
        visualYaw = yaw;
        visualPitch = pitch;

        // In the future, this would do something like:
        // visualPos = glm::mix(visualPos, predictedPos, 
        //                      1.0f - exp(-POSITION_SMOOTHING_FACTOR * deltaTime));
        // visualYaw = glm::mix(visualYaw, yaw,
        //                      1.0f - exp(-ROTATION_SMOOTHING_FACTOR * deltaTime));
        // visualPitch = glm::mix(visualPitch, pitch,
        //                        1.0f - exp(-ROTATION_SMOOTHING_FACTOR * deltaTime));
    }

    void ClientPlayer::ApplyServerCorrection(const glm::dvec3& pos, float newYaw, float newPitch) {
        // TODO: Implement server position correction with prediction reconciliation
        // For now, just accept the server position directly
        serverPos = pos;
        
        // Calculate prediction error
        glm::dvec3 error = serverPos - predictedPos;
        
        // If error is significant, snap to server position
        if (glm::length(error) > 0.1) {
            physics.position = serverPos;
            predictedPos = serverPos;
            Log::Debug("Applied server correction: error was (%.3f, %.3f, %.3f)",
                      error.x, error.y, error.z);
        }
        
        // Update rotation
        yaw = newYaw;
        pitch = newPitch;
    }

    glm::dvec3 ClientPlayer::GetEyePosition() const {
        if (IsSleeping()) {
            // The lying eye: no step-smoothing offset, the pose's own height.
            return physics.position + glm::dvec3(0.0, static_cast<double>(GetEyeHeight()), 0.0);
        }
        return physics.GetEyePosition();
    }

    void ClientPlayer::SetMorph(uint32_t code, float walkSpeed) {
        if (!Game::Morph::IsValid(code) || Game::Morph::KindOf(code) != Game::Morph::Kind::Block) {
            morphGridLock = false;   // the lock is a block's
        }
        if (!Game::Morph::IsValid(code) || Game::Morph::KindOf(code) != Game::Morph::Kind::Item) {
            heldByPlayer = 0;        // only an item is carried (the server lets go too)
        }
        if (!Game::Morph::IsValid(code)) {
            morph = Game::Morph::kNone;
            physics.ClearMorph();
            morphWalk.Stop();
            return;
        }
        const Game::Morph::Dims dims = Game::Morph::DimsOf(code);
        morph = code;
        physics.SetMorph(dims.width, dims.height, dims.eyeHeight, walkSpeed);
        physics.morphClimbsWalls = Game::Morph::ClimbsWalls(code);
        morphPrevPos = physics.position;
        morphBodyYaw = morphBodyYawOld = yaw;
    }

    void ClientPlayer::SetMorphGridLock(bool on) {
        morphGridLock = on && IsBlockMorph();
        morphLockBlockSeen = false;
        if (!morphGridLock) return;
        morphLockPos = glm::dvec3(std::floor(physics.position.x) + 0.5,
                                  std::round(physics.position.y),
                                  std::floor(physics.position.z) + 0.5);
        physics.position = morphLockPos;
        physics.velocity = glm::vec3(0.0f);
        physics.stepVisualOffset = 0.0f;
    }

    bool ClientPlayer::TickMorph(float headYaw) {
        if (!IsMorphed()) {
            morphPrevPos = physics.position;
            morphSwell = morphSwellOld = 0;
            morphBodyYaw = morphBodyYawOld = headYaw;
            return false;
        }
        const glm::dvec3 d = physics.position - morphPrevPos;
        morphPrevPos = physics.position;

        // MC LivingEntity.aiStep + tickHeadTurn (the rule the remote
        // copies use): moving, the body turns toward the travel direction
        // at 30 % a tick; still, it stays; the head may lead it by 50°.
        morphBodyYawOld = morphBodyYaw;
        const double speedSq = d.x * d.x + d.z * d.z;
        const float bodyTarget = speedSq > 0.0001 ? Game::Mth::YRotFromVector(glm::vec3(d)) : morphBodyYaw;
        morphBodyYaw += Game::Mth::WrapDegrees(bodyTarget - morphBodyYaw) * 0.3f;
        const float headOffset = Game::Mth::WrapDegrees(headYaw - morphBodyYaw);
        if (std::fabs(headOffset) > 50.0f) {
            morphBodyYaw += headOffset - std::copysign(50.0f, headOffset);
        }
        // MC LivingEntity.updateWalkAnimation: horizontal travel × 4, capped
        // at 1, smoothed by 0.4 a tick.
        float f = static_cast<float>(std::sqrt(d.x * d.x + d.z * d.z)) * 4.0f;
        if (f > 1.0f) f = 1.0f;
        morphWalk.Update(f, 0.4f, 1.0f);
        ++morphTicks;

        // The one-shot animations count down (MC EatBlockGoal.tick).
        morphAnimTicksOld = morphAnimTicks;
        if (morphAnimTicks > 0) --morphAnimTicks;

        // MC Creeper.tick: oldSwell = swell; swell += swellDir, clamped to
        // 0..maxSwell; the bang at maxSwell.
        bool boom = false;
        morphSwellOld = morphSwell;
        if (IsCreeperMorph()) {
            const int dir = (sneakPressed && jumpHeld) ? 1 : -1;
            morphSwell = std::clamp(morphSwell + dir, 0, Game::Morph::kCreeperSwellTicks);
            if (morphSwell >= Game::Morph::kCreeperSwellTicks) {
                boom = true;
                morphSwell = morphSwellOld = 0;
            }
        } else {
            morphSwell = 0;
        }
        return boom;
    }

    float ClientPlayer::GetEyeHeight() const {
        // MC LivingEntity.SLEEPING_DIMENSIONS: 0.2 × 0.2 with the eye at 0.2.
        if (IsSleeping()) return 0.2f * physics.scale;
        return physics.GetEyeHeight();
    }

    void ClientPlayer::SetJumpPressed(bool pressed) {
        // True key edge (jumpPressed below is consumed by physics each frame,
        // so it can't be used for tap detection — holding space re-registers).
        const bool risingEdge = pressed && !jumpKeyWasDown;
        jumpKeyWasDown = pressed;

        if (pressed && !jumpPressed) {
            jumpPressed = true; // Only register the press edge
        }

        // Double-tap-space creative flight toggle — MC LocalPlayer.aiStep
        // (LocalPlayer.java:760-782): first tap arms a 7-tick (0.35 s)
        // window; a second tap inside it flips abilities.flying. Gated on
        // mayFly; water bobbing and debug noclip keep their own controls.
        if (risingEdge && physics.mayFly && !physics.noclip &&
            (physics.isFlying || !physics.isInWater)) {
            if (flyToggleTimer > 0.0f) {
                physics.isFlying = !physics.isFlying;
                if (physics.isFlying) {
                    // Kill fall velocity so the toggle arrests the drop.
                    physics.velocity.y = 0.0f;
                }
                flyToggleTimer = 0.0f;
            } else {
                flyToggleTimer = FLY_DOUBLE_TAP_WINDOW;
            }
        }
    }

    void ClientPlayer::ToggleNoclip() {
        physics.noclip = !physics.noclip;
        Log::Info("Noclip %s", physics.noclip ? "enabled" : "disabled");

        if (physics.noclip) {
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
        }
    }

    void ClientPlayer::SetNoclip(bool enabled) {
        physics.noclip = enabled;
        Log::Info("Noclip %s", physics.noclip ? "enabled" : "disabled");

        if (physics.noclip) {
            physics.velocity = glm::vec3(0.0f);
            physics.isOnGround = false;
        }
    }

    void ClientPlayer::SelectSlot(int slot) {
        inventory.SetSelectedSlot(slot);
    }

    void ClientPlayer::SelectNextSlot() {
        inventory.SelectNextSlot();
    }

    void ClientPlayer::SelectPreviousSlot() {
        inventory.SelectPreviousSlot();
    }

    void ClientPlayer::UpdateStatistics(float deltaTime) {
        // Update play time
        stats.totalPlayTime += deltaTime;

        // Calculate distance traveled
        const float distanceThisFrame = static_cast<float>(glm::length(physics.position - lastPosition));
        stats.totalDistanceTraveled += distanceThisFrame;
        lastPosition = physics.position;
    }

} // namespace Game

namespace Client {

    void TickPlayerEffectParticles(const Game::ClientPlayer& localPlayer) {
        if (!g_clientMobManager) return;
        Game::EntityLevel& level = g_clientMobManager->Level();

        // The local player: its visuals are what the server would sync for it
        // (MC's own DATA_EFFECT_PARTICLES reaches the local player too).
        if (!localPlayer.activeEffects.empty() && localPlayer.health > 0) {
            const Game::EffectVisuals visuals = Game::EffectVisuals::FromEffects(
                localPlayer.activeEffects, localPlayer.HasEffect(Game::MobEffectId::Glowing));
            const float s = localPlayer.physics.scale;
            Game::SpawnEffectParticles(level, visuals, localPlayer.physics.position,
                                       localPlayer.physics.GetWidth(),
                                       localPlayer.physics.GetCurrentHeight() > 0.0f
                                           ? localPlayer.physics.GetCurrentHeight()
                                           : 1.8f * s);
        }

        if (!g_remotePlayerManager) return;
        for (const auto& [id, rp] : g_remotePlayerManager->GetPlayers()) {
            if (!rp.positionInitialized || rp.effects.particles.empty()) continue;
            if (!IsRemotePlayerInBoundLevel(rp)) continue;
            Game::SpawnEffectParticles(level, rp.effects, rp.position,
                                       0.6f * rp.scale, 1.8f * rp.scale);
        }
    }

} // namespace Client
