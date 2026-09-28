// File: src/server/items/FireworkItems.cpp
//
// Server half of the firework rocket and the crossbow: the bridges declared
// in common/world/level/FireworkLaunch.hpp. Design notes live there and in
// common/entity/FireworkItems.hpp.
//
// References (minecraft_code_26.3-pre-2/decompiled_net/minecraft/):
// world/item/FireworkRocketItem.java, world/item/CrossbowItem.java,
// world/item/ProjectileWeaponItem.java (draw / useAmmo / shoot),
// world/entity/player/Player.java (getProjectile), world/entity/projectile/
// Projectile.java (spawnProjectile, applyOnProjectileSpawned),
// world/entity/projectile/arrow/AbstractArrow.java (the weapon's pierce).

#include "common/world/level/FireworkLaunch.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/FireworkItems.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Leashable.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/FireworkRocket.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game::FireworkLaunch {

    namespace {

        Server::ServerPlayer* AsServerPlayer(IUsePlayer& player) {
            return dynamic_cast<Server::ServerPlayer*>(&player);
        }

        // The player's level and its entity there (the owner / shooter).
        struct Shooter {
            Server::ServerLevelBridge* level = nullptr;
            Server::PlayerEntityView*  view  = nullptr;
        };

        bool ResolveShooter(const Server::ServerPlayer& player, Shooter& out) {
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

        // MC CrossbowItem's startSoundPlayed / midLoadSoundPlayed. Fields of
        // the (singleton) item in MC; kept per player here so two players
        // drawing at once do not share one latch.
        struct DrawSounds {
            bool startPlayed = false;
            bool midPlayed   = false;
        };
        std::mutex& DrawSoundsMutex() {
            static std::mutex m;
            return m;
        }
        std::unordered_map<uint32_t, DrawSounds>& DrawSoundsByPlayer() {
            static std::unordered_map<uint32_t, DrawSounds> map;
            return map;
        }

        // MC Entity.calculateViewVector(xRot, yRot).
        glm::dvec3 ViewVector(float xRotDeg, float yRotDeg) {
            const double f = static_cast<double>(xRotDeg) * Mth::kDegToRad;
            const double g = -static_cast<double>(yRotDeg) * Mth::kDegToRad;
            return { std::sin(g) * std::cos(f), -std::sin(f), std::cos(g) * std::cos(f) };
        }

        // MC CrossbowItem.getShotPitch: the first shot at 1, the fan's
        // others high (odd) / low (even).
        float ShotPitch(JavaRandom& random, int index) {
            if (index == 0) return 1.0f;
            const float rangeDecider = (index & 1) == 1 ? 0.63f : 0.43f;
            return 1.0f / (random.NextFloat() * 0.5f + 1.8f) + rangeDecider;
        }

        // MC ProjectileWeaponItem.draw(weapon, player.getProjectile(weapon),
        // shooter) for the crossbow — the stacks it loads. Spends from the
        // inventory (useAmmo); a creative player's search that finds nothing
        // draws a plain arrow (Player.getProjectile's infinite-materials
        // fallback).
        std::vector<ItemStack> Draw(Server::ServerPlayer& player, Shooter& shooter, const ItemStack& crossbow) {
            Inventory& inventory = player.getInventory();
            const int slot = FireworkItems::FindCrossbowProjectileSlot(inventory);
            const bool creative = player.isCreative();
            if (slot < 0 && !creative) return {};
            ItemStack creativeArrow(Items::Arrow, 1);
            ItemStack& projectile = slot >= 0 ? inventory.MutableSlot(slot) : creativeArrow;
            if (projectile.IsEmpty()) return {};

            const int count = EnchantmentHelper::ProcessProjectileCount(*shooter.level, crossbow, *shooter.view, 1);
            const ItemStack projectileCopy = projectile;
            std::vector<ItemStack> drawn;
            drawn.reserve(static_cast<size_t>(std::max(count, 0)));
            for (int i = 0; i < count; ++i) {
                const bool forceInfinite = i > 0;
                const ItemStack& source = i == 0 ? projectile : projectileCopy;
                // useAmmo: infinite materials and the fan's copies cost
                // nothing; otherwise the weapon's ammo_use effects decide.
                const int ammoToUse = (!forceInfinite && !creative)
                    ? EnchantmentHelper::ProcessAmmoUse(*shooter.level, crossbow, source, 1) : 0;
                if (ammoToUse > source.count) continue;
                ItemStack used = source;
                if (ammoToUse == 0) {
                    used.count = 1;
                } else {
                    used.count = ammoToUse;
                    if (i == 0 && slot >= 0) {
                        projectile.count -= ammoToUse;
                        if (projectile.count <= 0) projectile.Clear();
                        player.markSlotDirty(slot);
                    }
                }
                drawn.push_back(std::move(used));
            }
            return drawn;
        }

    } // namespace

    void LaunchFromBlock(ILevelWrite* level, IUsePlayer* user, const ItemStack& stack, const glm::dvec3& at) {
        // new FireworkRocketEntity(level, player, x, y, z, itemStack), then
        // Projectile.spawnProjectile.
        Shooter shooter;
        Server::ServerPlayer* player = user ? AsServerPlayer(*user) : nullptr;
        EntityLevel* entities = nullptr;
        if (player && ResolveShooter(*player, shooter)) {
            entities = shooter.level;
        } else if (level) {
            entities = level->Entities();
        }
        if (!entities) return;
        auto rocket = std::make_unique<FireworkRocket>(entities);
        rocket->InitLaunch(at, stack);
        if (shooter.view) rocket->SetOwner(shooter.view);
        entities->AddFreshEntity(std::move(rocket));
    }

    bool BoostPlayer(IUsePlayer& user, uint32_t hand, const ItemStack& stack) {
        (void)hand;
        Server::ServerPlayer* player = AsServerPlayer(user);
        Shooter shooter;
        if (!player || !ResolveShooter(*player, shooter)) return false;
        // player.dropAllLeashConnections(null): the lead-break sound when
        // any lead came off.
        if (Leash::DropAllLeashConnections(*shooter.view, nullptr)) {
            shooter.level->PlaySoundFromEntity(nullptr, *shooter.view, SoundEvents::LEAD_BREAK,
                                               SoundSource::Neutral, 1.0f, 1.0f);
        }
        // new FireworkRocketEntity(level, itemStack, player): at, owned by and
        // attached to the glider.
        auto rocket = std::make_unique<FireworkRocket>(shooter.level);
        rocket->InitLaunch(shooter.view->position, stack);
        rocket->AttachTo(*shooter.view);
        shooter.level->AddFreshEntity(std::move(rocket));
        return true;
    }

    UseResult CrossbowBeginDraw(IUsePlayer& user, uint32_t hand) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return UseResult::Pass;
        // CrossbowItem.use: player.getProjectile(itemStack) non-empty (or
        // infinite materials) → startUsingItem, else FAIL.
        if (!FireworkItems::CanDrawCrossbow(player->getInventory(), player->isCreative())) {
            return UseResult::Fail;
        }
        {
            std::lock_guard<std::mutex> lock(DrawSoundsMutex());
            DrawSoundsByPlayer()[player->getPlayerId()] = DrawSounds{};
        }
        player->startUsingItem(hand);
        return UseResult::Consume;
    }

    void CrossbowUseTick(IUsePlayer& user, ItemStack& crossbow, int remainingTicks) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        Shooter shooter;
        if (!player || !ResolveShooter(*player, shooter)) return;

        // getChargingSounds: the highest Quick Charge's start (no middle), or
        // DEFAULT_SOUNDS; the end is always loading_end.
        const std::string quickStart = EnchantmentHelper::CrossbowChargingStartSound(crossbow);
        const std::string startSound = quickStart.empty() ? std::string(SoundEvents::CROSSBOW_LOADING_START)
                                                          : quickStart;
        const bool hasMid = quickStart.empty();

        const int chargeDuration = FireworkItems::CrossbowChargeDuration(crossbow);
        const int timeHeld = GetUseDuration(crossbow) - remainingTicks;
        const float tickPercent = chargeDuration > 0
            ? static_cast<float>(timeHeld) / static_cast<float>(chargeDuration)
            : 1.0f;
        const glm::dvec3 pos = shooter.view->position;

        bool playStart = false, playMid = false;
        {
            std::lock_guard<std::mutex> lock(DrawSoundsMutex());
            DrawSounds& sounds = DrawSoundsByPlayer()[player->getPlayerId()];
            if (tickPercent < FireworkItems::kCrossbowStartSoundPercent) {
                sounds.startPlayed = false;
                sounds.midPlayed = false;
            }
            if (tickPercent >= FireworkItems::kCrossbowStartSoundPercent && !sounds.startPlayed) {
                sounds.startPlayed = true;
                playStart = true;
            }
            if (tickPercent >= FireworkItems::kCrossbowMidSoundPercent && !sounds.midPlayed) {
                sounds.midPlayed = true;
                playMid = hasMid;
            }
        }
        if (playStart) {
            shooter.level->PlaySound(nullptr, pos, startSound, SoundSource::Players, 0.5f, 1.0f);
        }
        if (playMid) {
            shooter.level->PlaySound(nullptr, pos, SoundEvents::CROSSBOW_LOADING_MIDDLE, SoundSource::Players,
                                     0.5f, 1.0f);
        }

        // At full charge: tryLoadProjectiles (once — it is charged after).
        if (tickPercent >= 1.0f && !FireworkItems::IsCrossbowCharged(crossbow)) {
            std::vector<ItemStack> drawn = Draw(*player, shooter, crossbow);
            if (!drawn.empty()) {
                ChargedProjectiles charged;
                charged.items = std::move(drawn);
                FireworkItems::SetChargedProjectiles(crossbow, charged);
                JavaRandom& r = shooter.level->Random();
                shooter.level->PlaySound(nullptr, pos, SoundEvents::CROSSBOW_LOADING_END, SoundSource::Players,
                                         1.0f, 1.0f / (r.NextFloat() * 0.5f + 1.0f) + 0.2f);
            }
        }
    }

    void CrossbowShoot(IUsePlayer& user, uint32_t hand, ItemStack& crossbow) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        Shooter shooter;
        if (!player || !ResolveShooter(*player, shooter)) return;
        Server::ServerLevelBridge& level = *shooter.level;
        Server::PlayerEntityView& view = *shooter.view;

        // performShooting: weapon.set(CHARGED_PROJECTILES, EMPTY) returns the
        // old list, which is what flies.
        const auto loaded = crossbow.get(DataComponents::CHARGED_PROJECTILES);
        if (!loaded || loaded->IsEmpty()) return;
        const std::vector<ItemStack> projectiles = loaded->items;
        FireworkItems::SetChargedProjectiles(crossbow, ChargedProjectiles{});
        player->markSlotDirty(player->handSlotIndex(hand));

        // getShootingPower: a rocket anywhere in the load flies slower.
        const float power = loaded->Contains(Items::FireworkRocket) ? FireworkItems::kCrossbowFireworkPower
                                                                    : FireworkItems::kCrossbowArrowPower;
        constexpr float kUncertainty = 1.0f;
        const EquipmentSlot handSlot = hand == 1 ? EquipmentSlot::OFFHAND : EquipmentSlot::MAINHAND;

        // ProjectileWeaponItem.shoot: a fan of processProjectileSpread
        // degrees about the shooter's up vector.
        const int n = static_cast<int>(projectiles.size());
        const float maxAngle = EnchantmentHelper::ProcessProjectileSpread(level, crossbow, view, 0.0f);
        const float angleStep = n == 1 ? 0.0f : 2.0f * maxAngle / static_cast<float>(n - 1);
        const float angleOffset = static_cast<float>((n - 1) % 2) * angleStep / 2.0f;
        float direction = 1.0f;

        const float xRot = player->getPitch();
        const float yRot = player->getYaw();
        const glm::dvec3 viewVec = ViewVector(xRot, yRot);
        const glm::dvec3 upVec = ViewVector(xRot - 90.0f, yRot);   // Entity.calculateUpVector
        const glm::dvec3 pos = view.position;
        const double eyeY = pos.y + static_cast<double>(player->getEyeHeight());
        JavaRandom& random = level.Random();

        for (int i = 0; i < n; ++i) {
            const ItemStack& ammo = projectiles[static_cast<size_t>(i)];
            if (ammo.IsEmpty()) continue;
            const float angle = angleOffset + direction * static_cast<float>((i + 1) / 2) * angleStep;
            direction = -direction;

            // CrossbowItem.createProjectile.
            std::unique_ptr<Projectile> projectile;
            if (ammo.itemId == Items::FireworkRocket) {
                // new FireworkRocketEntity(level, projectile, shooter, x,
                // eyeY - 0.15, z, shotAtAngle = true).
                auto rocket = std::make_unique<FireworkRocket>(&level);
                rocket->InitLaunch(glm::dvec3(pos.x, eyeY - 0.15, pos.z), ammo);
                rocket->SetOwner(&view);
                rocket->SetShotAtAngle(true);
                projectile = std::move(rocket);
            } else {
                // ProjectileWeaponItem.createProjectile → ArrowItem.createArrow
                // (at the eye, 0.1 down), a player's shot always a crit; the
                // crossbow's arrow hits with CROSSBOW_HIT and carries the
                // weapon's Piercing (AbstractArrow's constructor).
                auto arrow = std::make_unique<Arrow>(&level);
                arrow->SetOwner(&view);
                if (ammo.itemId == Items::TippedArrow) arrow->SetPotionFromPickupStack(ammo);
                arrow->SetFiredFromWeapon(crossbow);
                const int pierce = EnchantmentHelper::GetPiercingCount(level, crossbow, ammo);
                if (pierce > 0) arrow->SetPierceLevel(pierce);
                arrow->SetCritArrow(true);
                arrow->SetSoundEvent(SoundEvents::CROSSBOW_HIT);
                // The pickup: the arrow shot, ALLOWED — CREATIVE_ONLY for an
                // INTANGIBLE_PROJECTILE (useAmmo's free copies: Multishot's
                // side arrows, anything drawn with infinite materials).
                arrow->SetPickupItemStack(ammo);
                arrow->SetPickup(i > 0 || player->isCreative() ? Arrow::Pickup::CreativeOnly
                                                               : Arrow::Pickup::Allowed);
                arrow->position = glm::dvec3(pos.x, eyeY - 0.1, pos.z);
                arrow->oldPosition = arrow->position;
                projectile = std::move(arrow);
            }

            // shootProjectile: the view vector turned `angle` degrees about
            // the up vector, shot at `power` with uncertainty 1.
            const glm::dquat turn = glm::angleAxis(static_cast<double>(angle) * Mth::kDegToRad,
                                                   glm::normalize(upVec));
            const glm::dvec3 shot = turn * viewVec;
            projectile->Shoot(shot.x, shot.y, shot.z, power, kUncertainty);
            level.PlaySound(nullptr, pos, SoundEvents::CROSSBOW_SHOOT, SoundSource::Players, 1.0f,
                            ShotPitch(random, i));

            // Projectile.spawnProjectile: add, then applyOnProjectileSpawned —
            // the launcher's projectile_spawned effects on an arrow (Flame).
            Projectile* spawned = projectile.get();
            level.AddFreshEntity(std::move(projectile));
            if (auto* arrow = dynamic_cast<Arrow*>(spawned)) {
                if (ItemStack* launcher = arrow->GetWeaponItem(); launcher && launcher->itemId != ammo.itemId) {
                    EnchantmentHelper::OnProjectileSpawned(level, *launcher, *arrow, &view,
                        [arrow](const ItemStack& broken) { arrow->OnItemBreak(broken); });
                }
            }

            // weapon.hurtAndBreak(getDurabilityUse(projectile)): a rocket
            // costs 3, an arrow 1; a broken crossbow stops the volley.
            HurtAndBreak(crossbow, ammo.itemId == Items::FireworkRocket ? 3 : 1, view, handSlot);
            if (crossbow.IsEmpty()) break;
        }
    }

} // namespace Game::FireworkLaunch
