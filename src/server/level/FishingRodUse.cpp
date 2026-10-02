// File: src/server/level/FishingRodUse.cpp
//
// Server half of `Game::UseFishingRodServer` (declared beside the hook in
// common/entity/projectile/FishingHook.hpp): MC FishingRodItem.use's server
// branch and FishingHook.retrieve.
//
// It lives on the server side of the common/server split for the reasons the
// ender pearl's bridge (EnderEyeThrow.cpp) does: the hook's owner must be the
// player's server-side entity view, the catch needs the player's LUCK, and the
// orbs go straight into this level's orb manager (MC spawns ONE orb of 1..6
// here, not ExperienceOrb.award's split).
//
// References: minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/item/
// FishingRodItem.java and world/entity/projectile/FishingHook.java (retrieve).

#include "ServerLevel.hpp"
#include "server/advancements/CriteriaTriggers.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/ExperienceOrbManager.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/ExperienceOrb.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/projectile/FishingHook.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace Game {

    namespace {

        // BuiltInLootTables.FISHING.
        constexpr const char* kFishingLootTable = "minecraft:gameplay/fishing";

        // The server objects behind the IUsePlayer: its level, the
        // ServerPlayer and that player's entity view there (the hook's owner).
        struct Fisher {
            Server::ServerLevel*       level = nullptr;
            Server::ServerLevelBridge* entityLevel = nullptr;
            Server::ServerPlayer*      player = nullptr;
            Server::PlayerEntityView*  view = nullptr;
        };

        bool ResolveFisher(IUsePlayer& player, Fisher& out) {
            auto* server = Server::g_integratedServer.get();
            if (!server) return false;
            out.level = server->GetLevel(DimensionFromRaw(player.getDimensionId()));
            if (!out.level || !out.level->Mobs() || !out.level->MobLevel()) return false;
            out.entityLevel = out.level->MobLevel();
            out.player = dynamic_cast<Server::ServerPlayer*>(&player);
            if (!out.player) return false;
            // Views are keyed by CONNECTION id — resolve through the session.
            auto* sessions = server->GetSessionManager();
            auto session = sessions ? sessions->GetSession(out.player->getPlayerId()) : nullptr;
            out.view = session ? out.entityLevel->GetPlayerView(session->GetConnectionId()) : nullptr;
            return out.view != nullptr;
        }

        // MC FishingHook.retrieve(rod) — how much the reel-in wears the rod.
        int Retrieve(FishingHook& hook, const Fisher& fisher, const ItemStack& rod) {
            LivingEntity* owner = hook.GetPlayerOwner();
            if (!owner || hook.ShouldStopFishing(*owner)) return 0;
            EntityLevel& level = *hook.Level();
            JavaRandom& random = level.Random();

            int dmg = 0;
            if (hook.HasHookedTarget()) {
                // Reel the catch in. The entity event lets the hooked
                // player's own client pull them (FishingHook.hpp).
                // CriteriaTriggers.FISHING_ROD_HOOKED with no catch; the
                // hooked-in context is the hooked entity (or the hook, for an
                // item entity — not an Entity here).
                Entity* hookedIn = hook.GetHookedIn();
                Server::CriteriaTriggers::FishingRodHooked(*fisher.player, rod, hookedIn ? *hookedIn : hook, {});
                hook.PullHooked();
                level.BroadcastEntityEvent(hook, FishingHook::kEventPullHooked);
                dmg = hook.GetHookedItemId() != 0 ? 3 : 5;
            } else if (hook.IsNibbling()) {
                // LootParams (FISHING set): ORIGIN = the hook, TOOL = the rod,
                // THIS_ENTITY = the hook (its open-water flag is what the
                // treasure pool's condition reads), luck = Luck of the Sea +
                // the player's LUCK attribute.
                ChestLoot::LootLevelContext context;
                context.dimensionId = static_cast<int>(level.Dimension());
                context.origin = hook.position;
                context.fishingHookInOpenWater = hook.IsOpenWaterFishing();
                const float luck = static_cast<float>(hook.GetLuck()) + fisher.player->getLuck();
                std::vector<ItemStack> items;
                if (!ChestLoot::GetRandomItems(kFishingLootTable, random, luck, items, &context)) {
                    Log::Warning("[Fishing] loot table %s is missing", kFishingLootTable);
                }
                // CriteriaTriggers.FISHING_ROD_HOOKED with the catch (MC also
                // counts FISH_CAUGHT — no statistics here).
                Server::CriteriaTriggers::FishingRodHooked(*fisher.player, rod, hook, items);
                for (const ItemStack& stack : items) {
                    if (stack.IsEmpty()) continue;
                    // The catch flies at the player: 0.1 of the gap, lifted by
                    // the fourth root of its squared length.
                    const double xa = owner->position.x - hook.position.x;
                    const double ya = owner->position.y - hook.position.y;
                    const double za = owner->position.z - hook.position.z;
                    const glm::dvec3 motion(xa * 0.1,
                                            ya * 0.1 + std::sqrt(std::sqrt(xa * xa + ya * ya + za * za)) * 0.08,
                                            za * 0.1);
                    // new ItemEntity(level, x, y, z, stack): no pickup delay.
                    level.SpawnThrownItem(hook.position, motion, stack, 0);

                    // new ExperienceOrb(level, owner.x, owner.y + 0.5,
                    // owner.z + 0.5, nextInt(6) + 1) — one orb, with the
                    // positional constructor's scatter.
                    if (Server::ExperienceOrbManager* orbs = fisher.level->Orbs()) {
                        ExperienceOrb orb;
                        orb.value = random.NextInt(6) + 1;
                        orb.pos = glm::dvec3(owner->position.x, owner->position.y + 0.5, owner->position.z + 0.5);
                        const double vx = (random.NextDouble() * 0.2 - 0.1) * 2.0;
                        const double vy = random.NextDouble() * 0.2 * 2.0;
                        const double vz = (random.NextDouble() * 0.2 - 0.1) * 2.0;
                        orb.vel = glm::dvec3(vx, vy, vz);
                        orb.needsSync = true;
                        orbs->Adopt(std::move(orb));
                    }
                }
                dmg = 1;
            }

            // Snagged on the ground: 2, whatever else happened.
            if (hook.onGround) dmg = 2;

            hook.Discard();
            return dmg;
        }

    } // namespace

    UseResult UseFishingRodServer(ILevelWrite* world, IUsePlayer& player, uint32_t hand, ItemStack& rod) {
        Fisher fisher;
        if (!world || !ResolveFisher(player, fisher)) return UseResult::Success;

        const auto soundPitch = [world]() {
            JavaRandom* r = world->Random();
            return 0.4f / ((r ? r->NextFloat() : 0.5f) * 0.4f + 0.8f);
        };
        const glm::dvec3 at = player.getPosition();
        const int32_t ownerId = fisher.view->GetId();

        // MC player.fishing. A hook left in the level the player walked out of
        // (that level stops ticking with nobody in it, so it never noticed)
        // is not this player's line any more.
        FishingHook* hook = FishingHook::FindForOwner(/*clientSide=*/false, ownerId);
        if (hook && hook->Level() != fisher.entityLevel) {
            hook->Discard();
            hook = nullptr;
        }

        if (hook) {
            const int dmg = Retrieve(*hook, fisher, rod);
            HurtAndBreak(rod, dmg, world, &player, hand);
            world->PlaySound(nullptr, at, SoundEvents::FISHING_BOBBER_RETRIEVE, SoundSource::Neutral,
                             1.0f, soundPitch());
            // itemStack.causeUseVibration(player, ITEM_INTERACT_FINISH).
            fisher.view->GameEvent(GameEventId::ItemInteractFinish);
        } else {
            world->PlaySound(nullptr, at, SoundEvents::FISHING_BOBBER_THROW, SoundSource::Neutral,
                             0.5f, soundPitch());
            EntityLevel& level = *fisher.entityLevel;
            // Lure's seconds as ticks; Luck of the Sea's levels.
            const int lureSpeed =
                static_cast<int>(EnchantmentHelper::GetFishingTimeReduction(level, rod, *fisher.view) * 20.0f);
            const int luck = EnchantmentHelper::GetFishingLuckBonus(level, rod, *fisher.view);

            auto created = std::make_unique<FishingHook>(&level);
            created->InitFromPlayer(*fisher.view, player.getYaw(), player.getPitch(), luck, lureSpeed);
            FishingHook* spawned = created.get();
            // Projectile.spawnProjectile: addFreshEntity, then
            // applyOnProjectileSpawned (the rod's projectile_spawned effects).
            if (fisher.level->Mobs()->Add(std::move(created)) != 0) {
                const EquipmentSlot slot = hand == 1 ? EquipmentSlot::OFFHAND : EquipmentSlot::MAINHAND;
                EnchantmentHelper::OnProjectileSpawned(level, rod, *spawned, fisher.view,
                                                       [&player, slot](const ItemStack& broken) {
                                                           player.OnEquippedItemBroken(broken, slot);
                                                       });
            } else {
                Log::Warning("[Fishing] could not register the cast hook");
            }
            // (Stats.ITEM_USED — no statistics here.)
            fisher.view->GameEvent(GameEventId::ItemInteractStart);
        }
        return UseResult::Success;
    }

} // namespace Game
