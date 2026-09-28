// File: src/server/items/TridentSpearItems.cpp
//
// Server half of the trident and the spears (common/entity/WeaponItems.hpp):
// TridentItem's draw, throw and riptide; the spear's charge and jab; and the
// player-specific combat those reach through the player's entity view —
// Player.stabAttack, the riptide's touch attack (checkAutoSpinAttack) — as
// PlayerEntityView members.
#include "common/entity/WeaponItems.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/entity/ShoulderEntities.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/SpearItem.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace Game::WeaponItems {

    namespace {

        Server::ServerPlayer* AsServerPlayer(IUsePlayer& player) {
            return dynamic_cast<Server::ServerPlayer*>(&player);
        }

        // The player's level and its entity there.
        struct Holder {
            Server::ServerLevelBridge* level = nullptr;
            Server::PlayerEntityView*  view  = nullptr;
        };

        bool Resolve(const Server::ServerPlayer& player, Holder& out) {
            Server::IntegratedServer* server = Server::g_integratedServer.get();
            if (!server) return false;
            Server::PlayerSessionManager* sessions = server->GetSessionManager();
            auto session = sessions ? sessions->GetSession(player.getPlayerId()) : nullptr;
            Server::ServerLevel* level = server->GetLevel(DimensionFromRaw(player.getDimensionId()));
            if (!session || !level || !level->MobLevel()) return false;
            out.level = level->MobLevel();
            out.view = out.level->GetPlayerView(session->GetConnectionId());
            return out.view != nullptr;
        }

        EquipmentSlot SlotOfHand(uint32_t hand) {
            return hand == 1 ? EquipmentSlot::OFFHAND : EquipmentSlot::MAINHAND;
        }

    } // namespace

    // ── Spears ───────────────────────────────────────────────────────────

    UseResult SpearBegin(IUsePlayer& user, uint32_t hand) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return UseResult::Pass;
        const ItemStack& stack = player->getItemInHand(hand);
        const Spear::KineticWeapon* kinetic = Spear::Kinetic(stack);
        if (!kinetic) return UseResult::Pass;
        // Item.use: player.startUsingItem(hand); kineticWeapon.makeSound(player).
        player->startUsingItem(hand);
        Holder holder;
        if (Resolve(*player, holder)) Spear::PlayWeaponSound(*holder.view, kinetic->sound);
        return UseResult::Consume;
    }

    void SpearUseTick(IUsePlayer& user, ItemStack& spear, int remainingTicks) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        const Spear::KineticWeapon* kinetic = Spear::Kinetic(spear);
        if (!player || !kinetic) return;
        Holder holder;
        if (!Resolve(*player, holder)) return;
        // ItemStack.onUseTick → damageEntities(stack, ticksRemaining, player,
        // getUsedItemHand().asEquipmentSlot()).
        Spear::DamageEntities(*kinetic, GetUseDuration(spear), remainingTicks, *holder.view,
                              SlotOfHand(player->getUsedItemHand()));
    }

    void Stab(IUsePlayer& user) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player || player->getGameMode() == Server::GameMode::SPECTATOR) return;
        const ItemStack& held = player->getItemInHand(0);
        // cannotAttackWithItem(itemInHand, 5): the charge (with five ticks of
        // slack for the round trip) must reach MINIMUM_ATTACK_CHARGE.
        if (Spear::IsSpear(held.itemId) &&
            player->getAttackStrengthScale(5.0f) < Spear::kMinimumAttackCharge) {
            return;
        }
        const Spear::PiercingWeapon* piercing = Spear::Piercing(held);
        if (!piercing) return;
        Holder holder;
        if (!Resolve(*player, holder)) return;
        Spear::PiercingAttack(*piercing, *holder.view, EquipmentSlot::MAINHAND);
    }

    // ── Trident ──────────────────────────────────────────────────────────

    bool CanReleaseTrident(const ItemStack& trident, int ticksHeld, bool inWaterOrRain, bool passenger) {
        if (ticksHeld < ThrownTrident::kThrowThreshold) return false;
        const float riptide = EnchantmentHelper::GetTridentSpinAttackStrength(trident);
        if (riptide > 0.0f && (!inWaterOrRain || passenger)) return false;
        return !NextDamageWillBreak(trident);
    }

    UseResult TridentBegin(IUsePlayer& user, uint32_t hand) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return UseResult::Pass;
        const ItemStack& stack = player->getItemInHand(hand);
        // TridentItem.use.
        if (NextDamageWillBreak(stack)) return UseResult::Fail;
        if (EnchantmentHelper::GetTridentSpinAttackStrength(stack) > 0.0f) {
            Holder holder;
            if (!Resolve(*player, holder) || !holder.view->IsInWaterOrRain()) return UseResult::Fail;
        }
        player->startUsingItem(hand);
        return UseResult::Consume;
    }

    void TridentRelease(IUsePlayer& user, ItemStack& trident, int remainingTicks) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return;
        Holder holder;
        if (!Resolve(*player, holder)) return;
        Server::PlayerEntityView& view = *holder.view;
        Server::ServerLevelBridge& level = *holder.level;

        const int ticksHeld = GetUseDuration(trident) - remainingTicks;
        if (!CanReleaseTrident(trident, ticksHeld, view.IsInWaterOrRain(), player->isPassenger())) return;
        const float riptide = EnchantmentHelper::GetTridentSpinAttackStrength(trident);

        // pickHighestLevel(TRIDENT_SOUND) — Riptide's riptide_N — else
        // TRIDENT_THROW.
        std::string sound = EnchantmentHelper::TridentSound(trident);
        if (sound.empty()) sound = SoundEvents::TRIDENT_THROW;

        // itemStack.hurtWithoutBreaking(1, player).
        HurtWithoutBreaking(trident, 1, level.Random(), player->isCreative());

        if (riptide == 0.0f) {
            // consumeAndReturn(1, player): the thrown copy; the hand keeps
            // it only with infinite materials.
            ItemStack thrown = trident;
            thrown.count = 1;
            if (!player->isCreative()) {
                trident.count -= 1;
                if (trident.count <= 0) trident.Clear();
            }
            // Projectile.spawnProjectileFromRotation(ThrownTrident::new, level,
            // thrown, player, 0, 2.5, 1): from the eyes (0.1 down), along the
            // look, the player's own motion added.
            auto entity = std::make_unique<ThrownTrident>(&level);
            entity->SetOwner(&view);
            entity->position = glm::dvec3(view.position.x, view.GetEyeY() - 0.1, view.position.z);
            entity->oldPosition = entity->position;
            entity->SetTridentItem(thrown);
            // A player's throw is ALLOWED; with infinite materials only a
            // creative player may sweep it up.
            entity->SetPickup(player->isCreative() ? Arrow::Pickup::CreativeOnly : Arrow::Pickup::Allowed);
            entity->ShootFromRotation(view, player->getPitch(), player->getYaw(), 0.0f,
                                      ThrownTrident::kShootPower, 1.0f);
            ThrownTrident* spawned = entity.get();
            level.AddFreshEntity(std::move(entity));
            // level.playSound(null, trident, sound, PLAYERS, 1, 1).
            level.PlaySoundFromEntity(nullptr, *spawned, sound, SoundSource::Players, 1.0f, 1.0f);
            return;
        }

        // The riptide, server half: the spin (20 ticks at 8 damage with this
        // trident) and the sound for everyone. The push and the lift are the
        // client's own movement (it predicted them on release).
        player->startAutoSpinAttack(20, 8.0f, player->getUsedItemHand(), trident);
        // Player.startAutoSpinAttack, server side: the shoulder parrots come
        // off.
        Server::ShoulderEntities::RemoveEntitiesOnShoulder(*player);
        level.PlaySoundFromEntity(nullptr, view, sound, SoundSource::Players, 1.0f, 1.0f);
    }

} // namespace Game::WeaponItems

namespace Server {

    // ── PlayerEntityView: the player's side of spear and riptide combat ──

    float PlayerEntityView::GetJabAttackDamage() const {
        // ATTACK_DAMAGE as the player has it: base 1, the main hand's
        // modifier, Strength / Weakness.
        if (!m_player) return Game::kPlayerBaseAttackDamage;
        const Game::ItemStack& held = m_player->getItemInHand(0);
        float itemDamage = 0.0f, itemSpeed = 0.0f;
        Game::GetItemAttackAttributes(held.itemId, itemDamage, itemSpeed);
        return m_player->getAttackDamage(itemDamage);
    }

    void PlayerEntityView::OnAttack() {
        // Player.onAttack → resetOnlyAttackStrengthTicker.
        if (m_player) m_player->resetAttackStrengthTicker();
    }

    int PlayerEntityView::GetFoodLevel() const {
        return m_player ? m_player->getFood() : 20;
    }

    bool PlayerEntityView::StabAttack(Game::EquipmentSlot slot, Game::Entity& target, float baseDamage,
                                      bool dealsDamage, bool dealsKnockback, bool dismounts) {
        // MC Player.stabAttack.
        if (!m_player || !m_level || m_level->IsClientSide()) return false;
        Game::LivingEntity* living = target.AsLiving();
        // cannotAttack: !isAttackable || skipAttackInteraction.
        if (living && !living->IsAttackable()) return false;
        if (target.SkipAttackInteraction(*this)) return false;

        Game::ItemStack* weapon = EquipmentInSlot(slot);
        const bool spear = weapon && !weapon->IsEmpty() && Game::Spear::IsSpear(weapon->itemId);
        const Game::MobDamageSource sourceType =
            spear ? Game::MobDamageSource::Spear : Game::MobDamageSource::PlayerAttack;
        const Game::DamageSourceInfo source = Game::DamageSourceInfo::Of(sourceType, this, nullptr);

        // magicBoost = getEnchantedDamage(target, base, source) - base; both
        // scale with the attack charge unless this is the used hand's charge.
        float magicBoost = 0.0f;
        if (weapon && !weapon->IsEmpty()) {
            magicBoost = Game::EnchantmentHelper::ModifyDamage(*m_level, *weapon, target, source, baseDamage) -
                         baseDamage;
        }
        const uint32_t usedHand = m_player->getUsedItemHand();
        const Game::EquipmentSlot usedSlot = usedHand == 1 ? Game::EquipmentSlot::OFFHAND
                                                           : Game::EquipmentSlot::MAINHAND;
        if (!m_player->isUsingItem() || usedSlot != slot) {
            const float scale = m_player->getAttackStrengthScale(0.5f);
            magicBoost *= scale;
            baseDamage *= 0.2f + scale * scale * 0.8f;
        }

        const float totalDamage = dealsDamage ? baseDamage + magicBoost : 0.0f;
        const float oldHealth = living ? living->GetHealth() : 0.0f;
        bool wasHurt = false;
        if (dealsDamage && living) {
            if (auto* dragon = dynamic_cast<Game::EnderDragon*>(living)) {
                wasHurt = dragon->HurtPart(sourceType, totalDamage, this, /*headHit=*/false, this);
            } else {
                wasHurt = living->Hurt(sourceType, totalDamage, this);
            }
        }
        if (dealsKnockback && living) {
            // causeExtraKnockback(target, 0.4) then (target, getKnockback):
            // along the player's facing (the (sin, -cos) Knockback wants).
            const float yaw = yRot * Game::Mth::kDegToRad;
            living->Knockback(0.4, std::sin(yaw), -std::cos(yaw));
            float knockback = static_cast<float>(GetAttributeValue(Game::Attribute::AttackKnockback));
            if (weapon && !weapon->IsEmpty()) {
                knockback = Game::EnchantmentHelper::ModifyKnockback(*m_level, *weapon, target, source, knockback);
            }
            knockback /= 2.0f;
            if (knockback > 0.0f) living->Knockback(knockback, std::sin(yaw), -std::cos(yaw));
        }
        bool dismounted = false;
        if (dismounts && target.IsPassenger()) {
            dismounted = true;
            target.StopRiding();
        }
        if (!wasHurt && !dealsKnockback && !dismounted) return false;

        // attackVisualEffects(target, false, false, dealsDamage, stab=true,
        // magicBoost): no swing sound for a stab, the enchanted-hit burst
        // when the enchantments added damage.
        if (living && magicBoost > 0.0f && g_integratedServer) {
            g_integratedServer->BroadcastAttackEffects(m_level->Dimension(), *living, false, true);
        }
        SetLastHurtMob(&target);
        // itemAttackInteraction(target, weapon, source, wasHurt): the wear,
        // then the post-attack effects when it hurt.
        if (weapon && !weapon->IsEmpty() && living) {
            Game::HurtEnemy(*weapon, *this);
            if (wasHurt) {
                Game::EnchantmentHelper::DoPostAttackEffectsWithItemSource(*m_level, target, source, weapon);
            }
        }
        // damageStatsAndHearts: DAMAGE_INDICATOR hearts for more than a
        // heart dealt.
        if (living) {
            const float actual = oldHealth - living->GetHealth();
            if (actual > 2.0f) {
                const int count = static_cast<int>(static_cast<double>(actual) * 0.5);
                m_level->SendParticles(Game::ParticleOptions(Game::ParticleKind::DamageIndicator), false, false,
                                       target.position.x, target.position.y + target.GetBbHeight() * 0.5,
                                       target.position.z, count, 0.1, 0.0, 0.1, 0.2);
            }
        }
        // causeFoodExhaustion(0.1) — not for an invulnerable (creative /
        // spectator) player.
        const GameMode mode = m_player->getGameMode();
        if (mode == GameMode::SURVIVAL || mode == GameMode::ADVENTURE) {
            m_player->getFoodData().addExhaustion(0.1f);
        }
        return true;
    }

    void PlayerEntityView::TickAutoSpinAttack() {
        // MC LivingEntity.aiStep: `if (autoSpinAttackTicks > 0) {
        // --autoSpinAttackTicks; checkAutoSpinAttack(before, now); }`, for
        // the box the client's move swept this tick. The SPIN_ATTACK pose's
        // 0.6 x 0.6 box (Player.POSES).
        if (!m_player) return;
        if (m_player->getAutoSpinAttackTicks() <= 0) {
            if (m_player->isAutoSpinAttack()) m_player->stopAutoSpinAttack();
            return;
        }
        m_player->setAutoSpinAttackTicks(m_player->getAutoSpinAttackTicks() - 1);

        const double half = 0.3 * static_cast<double>(scale);
        const double height = 0.6 * static_cast<double>(scale);
        const glm::dvec3 lo = glm::min(position, oldPosition);
        const glm::dvec3 hi = glm::max(position, oldPosition);
        Game::AABB swept;
        swept.min = glm::vec3(lo - glm::dvec3(half, 0.0, half));
        swept.max = glm::vec3(hi + glm::dvec3(half, height, half));

        std::vector<Game::Entity*> entities;
        if (m_level) m_level->GetEntitiesInBox(swept, this, entities);
        for (Game::Entity* e : entities) {
            Game::LivingEntity* living = e ? e->AsLiving() : nullptr;
            if (!living || !living->IsAlive()) continue;
            // A boat or a minecart is no LivingEntity in MC.
            if (Game::IsVehicleEntityType(living->GetType())) continue;
            // doAutoAttackOnTouch → Player.attack(entity), then the spin ends
            // (the client bounces off on its own copy).
            m_player->setAutoSpinAttackTicks(0);
            if (g_integratedServer && !dynamic_cast<Game::EnderDragon*>(living)) {
                g_integratedServer->HandleInteract(static_cast<uint32_t>(GetId()), living->GetId(),
                                                   /*attack=*/true, m_player->isSprinting(), -1, nullptr,
                                                   /*autoSpinAttack=*/true);
            }
            break;
        }
        if (m_player->getAutoSpinAttackTicks() <= 0) m_player->stopAutoSpinAttack();
    }

} // namespace Server
