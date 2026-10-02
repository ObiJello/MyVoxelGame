// File: src/common/entity/ConsumableBehavior.cpp
// See header. MC citations per function. Sounds play through the player's
// level (Level.playSound); particles and game events are still stubs.
#include "ConsumableBehavior.hpp"

#include "../core/Log.hpp"
#include "Inventory.hpp"
#include "../world/level/WorldDrops.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/advancements/CriteriaTriggers.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/physics/Physics.hpp"
#include "common/data/DataComponents.hpp"

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace Game::ConsumableBehavior {

    // Mirrors Consumable.startConsuming — Consumable.java:39-52.
    UseResult StartConsuming(World* world, Server::ServerPlayer& player,
                             uint32_t hand, ItemStack& stack) {
        auto consumable = stack.get(DataComponents::CONSUMABLE);
        if (!consumable) return UseResult::Pass;

        if (!CanConsume(player, stack)) {
            return UseResult::Fail;                       // :40-41
        }
        const bool consumesOverTime = consumable->consumeTicks() > 0;  // :43
        if (consumesOverTime) {
            player.startUsingItem(hand);                  // :45
            return UseResult::Consume;                    // :46
        }
        // Instant consume (:48-49) — no vanilla food uses 0 s, but the path
        // is kept for parity. Result lands via in-place mutation.
        OnConsume(world, player, stack, *consumable);
        return UseResult::Consume;
    }

    // Mirrors Consumable.canConsume — Consumable.java:72-79.
    bool CanConsume(const Server::ServerPlayer& player, const ItemStack& stack) {
        auto food = stack.get(DataComponents::FOOD);
        if (food) {
            // Player.canEat(canAlwaysEat) = canAlwaysEat || foodData.needsFood()
            return player.canEat(food->canAlwaysEat);
        }
        return true;   // non-food consumables (potions) always drinkable
    }

    namespace {
        // MC Consumable.emitParticlesAndSounds' sound half: the consume sound
        // through user.playSound — Player.playSound, i.e. for everyone but
        // the eater, whose client plays its own copy while it predicts the
        // use (ClientPlayerController::UpdateUsingTick). Eating is 0.5 or 1.0
        // loud at a triangle(1, 0.2) pitch; drinking 0.5 at 0.9..1.0.
        void EmitConsumeSound(Server::ServerPlayer& player, const Consumable& consumable) {
            World* world = player.soundWorld();
            if (!world || consumable.sound.empty()) return;
            JavaRandom& r = player.soundRandom();
            const float eatVolume   = r.NextBool() ? 0.5f : 1.0f;
            const float eatPitch    = 1.0f + 0.2f * (r.NextFloat() - r.NextFloat());
            const float drinkPitch  = 0.9f + r.NextFloat() * 0.1f;   // Mth.randomBetween(0.9, 1.0)
            const bool  drink = consumable.animation == ItemUseAnimation::DRINK;
            world->PlaySound(&player, player.getPosition(), consumable.sound, SoundSource::Players,
                             drink ? 0.5f : eatVolume, drink ? drinkPitch : eatPitch);
        }

    } // namespace

    namespace {
        // MC LivingEntity.randomTeleport(x, y, z, broadcast, forbidden):
        // drop to the first motion-blocking block below, refuse a landing on
        // #consumable_does_not_teleport_to, then the body must fit there
        // without touching a liquid; entity event 46 (the portal puff).
        bool RandomTeleport(LivingEntity& user, const glm::dvec3& target,
                            const std::function<void(const glm::dvec3&)>& moveUser) {
            EntityLevel* level = user.Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            if (!blocks) return false;
            glm::ivec3 pos(static_cast<int>(std::floor(target.x)), static_cast<int>(std::floor(target.y)),
                           static_cast<int>(std::floor(target.z)));
            double y = target.y;
            bool landed = false;
            BlockID below = BlockID::Air;
            while (!landed && pos.y > level->GetMinY()) {
                below = blocks->GetBlock(pos.x, pos.y - 1, pos.z);
                if (BlockRegistry::HasCollision(below)) landed = true;
                else { y -= 1.0; --pos.y; }
            }
            if (!landed) return false;
            if (DataTags::HasTag(DataTags::Registry::Block, BlockRegistry::Get(below).registrySlug,
                                 "#minecraft:consumable_does_not_teleport_to")) {
                return false;
            }
            const glm::dvec3 destination(target.x, y, target.z);
            const glm::vec3 half = user.HalfExtents();
            const AABB box(glm::vec3(destination.x, destination.y + half.y, destination.z), half * 2.0f);
            if (CollidesAt(box, level->Physics())) return false;
            const int x0 = static_cast<int>(std::floor(destination.x - half.x));
            const int x1 = static_cast<int>(std::floor(destination.x + half.x));
            const int y1 = static_cast<int>(std::floor(destination.y + half.y * 2.0f));
            const int z0 = static_cast<int>(std::floor(destination.z - half.z));
            const int z1 = static_cast<int>(std::floor(destination.z + half.z));
            for (int bx = x0; bx <= x1; ++bx) {
                for (int by = pos.y; by <= y1; ++by) {
                    for (int bz = z0; bz <= z1; ++bz) {
                        if (blocks->ContainsWater(bx, by, bz)) return false;
                        const BlockID b = blocks->GetBlock(bx, by, bz);
                        if (b == BlockID::Lava) return false;
                    }
                }
            }
            if (moveUser) moveUser(destination);
            user.position = destination;
            user.velocity = glm::dvec3(0.0);
            user.needsSync = true;
            level->BroadcastEntityEvent(user, 46);
            return true;
        }

        bool TeleportRandomly(LivingEntity& user, const ConsumeEffect& effect,
                              const std::function<void(const glm::dvec3&)>& moveUser) {
            EntityLevel* level = user.Level();
            if (!level) return false;
            JavaRandom& random = level->Random();
            const double d = static_cast<double>(effect.diameter);
            for (int attempt = 0; attempt < 16; ++attempt) {
                const double xx = user.position.x + (random.NextDouble() - 0.5) * d;
                const double yy = std::clamp(user.position.y + (random.NextDouble() - 0.5) * d,
                                             static_cast<double>(level->GetMinY()),
                                             static_cast<double>(level->GetMaxY()));
                const double zz = user.position.z + (random.NextDouble() - 0.5) * d;
                if (user.IsPassenger()) user.StopRiding();
                const glm::dvec3 oldPos = user.position;
                if (!RandomTeleport(user, glm::dvec3(xx, yy, zz), moveUser)) continue;
                if (ILevelWrite* write = level->MutableBlocks()) {
                    write->GameEvent(GameEventId::Teleport, oldPos, GameEventContext::Of(&user));
                }
                const bool fox = user.GetType() == EntityTypeId::Fox;
                level->PlaySound(nullptr, user.position, fox ? SoundEvents::FOX_TELEPORT : SoundEvents::CHORUS_FRUIT_TELEPORT,
                                 fox ? SoundSource::Neutral : SoundSource::Players, 1.0f, 1.0f);
                if (effect.directionalParticles) {
                    const glm::ivec3 origin(static_cast<int>(std::floor(oldPos.x)), static_cast<int>(std::floor(oldPos.y)),
                                            static_cast<int>(std::floor(oldPos.z)));
                    level->PlayLevelEvent(nullptr, LevelEvent::PARTICLES_CONSUME_EFFECT_TELEPORT, origin,
                                          ClampedPackDifferenceInPosition(origin, user.BlockPosition(), 127, 127, 127));
                }
                user.ResetFallDistance();
                return true;
            }
            return false;
        }
    } // namespace

    bool ApplyConsumeEffect(LivingEntity& user, const ConsumeEffect& effect,
                            const std::function<void(const glm::dvec3&)>& moveUser) {
        EntityLevel* level = user.Level();
        if (!level || level->IsClientSide()) return false;
        switch (effect.type) {
            case ConsumeEffect::Type::ApplyStatusEffects: {
                // ApplyStatusEffectsConsumeEffect.apply: one roll gates the
                // whole list.
                if (level->Random().NextFloat() >= effect.probability) return false;
                bool any = false;
                for (const MobEffectInstance& e : effect.effects) any |= user.AddEffect(MobEffectInstance(e));
                return any;
            }
            case ConsumeEffect::Type::RemoveStatusEffects: {
                // RemoveStatusEffectsConsumeEffect: each effect of the set.
                bool any = false;
                for (const std::string& entry : effect.removeEffects) {
                    if (!entry.empty() && entry[0] == '#') {
                        for (int i = 0; i < kMobEffectCount; ++i) {
                            const auto id = static_cast<MobEffectId>(i);
                            if (DataTags::HasTag(DataTags::Registry::MobEffect, GetEffectName(id), entry)) {
                                any |= user.RemoveEffect(id);
                            }
                        }
                        continue;
                    }
                    MobEffectId id;
                    if (ParseEffectId(entry, id)) any |= user.RemoveEffect(id);
                }
                return any;
            }
            case ConsumeEffect::Type::ClearAllStatusEffects:
                return user.RemoveAllEffects();
            case ConsumeEffect::Type::TeleportRandomly:
                return TeleportRandomly(user, effect, moveUser);
            case ConsumeEffect::Type::PlaySound:
                // PlaySoundConsumeEffect: level.playSound(null, user, sound,
                // user.getSoundSource(), 1, 1).
                if (effect.sound.empty()) return false;
                level->PlaySound(nullptr, user.position, effect.sound, user.GetSoundSource(), 1.0f, 1.0f);
                return true;
        }
        return false;
    }

    // Mirrors Consumable.onConsume — Consumable.java:54-70.
    void OnConsume(World* world, Server::ServerPlayer& player,
                   ItemStack& stack, const Consumable& consumable) {
        (void)world;

        // :56 emitParticlesAndSounds(random, user, stack, 16 particles): the
        // sound for everyone else; the eater's client spawns the crumbs
        // (has_consume_particles) and its own sound while it predicts.
        EmitConsumeSound(player, consumable);

        // :58-59 awardStat (no statistics here) + CriteriaTriggers.CONSUME_ITEM.
        Server::CriteriaTriggers::ConsumeItem(player, stack);

        // :62 ConsumableListener components — stack.getAllOfType(
        // ConsumableListener.class): FOOD, POTION_CONTENTS and
        // SUSPICIOUS_STEW_EFFECTS are the three this engine carries.
        //
        // FOOD (FoodProperties.onConsume, FoodProperties.java:26-34):
        // restore hunger/saturation + the burp.
        if (auto food = stack.get(DataComponents::FOOD)) {
            player.getFoodData().eatFinal(food->nutrition, food->saturation);
            // FoodProperties.onConsume: the consume sound again for everyone
            // (NEUTRAL, pitch triangle(1, 0.4)), then the burp (PLAYERS, 0.5,
            // 0.9..1.0) — both with except = null, the eater included.
            if (World* world = player.soundWorld()) {
                JavaRandom& r = player.soundRandom();
                if (!consumable.sound.empty()) {
                    world->PlaySound(nullptr, player.getPosition(), consumable.sound, SoundSource::Neutral,
                                     1.0f, 1.0f + 0.4f * (r.NextFloat() - r.NextFloat()));
                }
                world->PlaySound(nullptr, player.getPosition(), SoundEvents::PLAYER_BURP, SoundSource::Players,
                                 0.5f, 0.9f + r.NextFloat() * 0.1f);
            }
        }
        // POTION_CONTENTS (PotionContents.onConsume): applyToLivingEntity at
        // the stack's POTION_DURATION_SCALE — instantaneous effects (healing,
        // harming) land at once through applyInstantaneousEffect, the rest
        // through addEffect. A water bottle has no effects and does nothing.
        if (auto potion = stack.get(DataComponents::POTION_CONTENTS)) {
            if (LivingEntity* user = player.effectEntity()) {
                potion->ApplyToLivingEntity(
                    *user, stack.get(DataComponents::POTION_DURATION_SCALE).value_or(1.0f));
            }
        }
        // SUSPICIOUS_STEW_EFFECTS (SuspiciousStewEffects.onConsume): each
        // entry's createEffectInstance through addEffect.
        if (auto stew = stack.get(DataComponents::SUSPICIOUS_STEW_EFFECTS)) {
            for (const auto& entry : stew->effects) {
                player.addEffect(entry.CreateEffectInstance());
            }
        }
        // OMINOUS_BOTTLE_AMPLIFIER (OminousBottleAmplifier.onConsume): Bad
        // Omen at the bottle's level for 100 minutes — not ambient, no
        // particles, an icon.
        if (auto amplifier = stack.get(DataComponents::OMINOUS_BOTTLE_AMPLIFIER)) {
            player.addEffect(MobEffectInstance(MobEffectId::BadOmen, DataComponents::kOminousBottleEffectDuration,
                                               *amplifier, /*ambient=*/false, /*visible=*/false,
                                               /*showIcon=*/true));
        }

        // :63-65 onConsumeEffects.forEach(apply) — server side, which this
        // always is. The status-effect kinds run through the player's
        // LivingEntity (ServerPlayer::addEffect → PlayerEntityView), exactly
        // MC's user.addEffect / removeEffect / removeAllEffects.
        if (LivingEntity* user = player.effectEntity()) {
            Server::ServerPlayer* owner = &player;
            const auto move = [owner](const glm::dvec3& to) { owner->teleport(to); };
            for (const auto& effect : consumable.onConsumeEffects) ApplyConsumeEffect(*user, effect, move);
        }

        // :67 user.gameEvent(animation == DRINK ? DRINK : EAT) — at the
        // player, sourced from them (their PlayerEntityView).
        if (Entity* user = player.GameEventSource()) {
            user->GameEvent(consumable.animation == ItemUseAnimation::DRINK ? GameEventId::Drink
                                                                            : GameEventId::Eat);
        }

        // :68 stack.consume(1, user) — ItemStack.consume skips the shrink for
        // hasInfiniteMaterials (creative).
        if (player.getGameMode() != Server::GameMode::CREATIVE) {
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
        }
    }

    // Mirrors Consumable.shouldEmitParticlesAndSounds — Consumable.java:107-112.
    bool ShouldEmitParticlesAndSounds(const Consumable& consumable,
                                      int useItemRemainingTicks) {
        const int itemUsedForTicks = consumable.consumeTicks() - useItemRemainingTicks;
        const int waitTicks = static_cast<int>(
            static_cast<float>(consumable.consumeTicks()) * 0.21875f); // CONSUME_EFFECTS_START_FRACTION
        const bool isValidTime = itemUsedForTicks > waitTicks;
        return isValidTime && useItemRemainingTicks % 4 == 0;  // CONSUME_EFFECTS_INTERVAL
    }

    // Mirrors ItemStack.onUseTick's CONSUMABLE branch — ItemStack.java:1060-1064.
    void OnUseTick(Server::ServerPlayer& player, const ItemStack& stack,
                   int remainingTicks) {
        auto consumable = stack.get(DataComponents::CONSUMABLE);
        if (consumable && ShouldEmitParticlesAndSounds(*consumable, remainingTicks)) {
            // :1063 emitParticlesAndSounds(…, 5 particles) — the sound half
            // (the particles are the eater's client's).
            EmitConsumeSound(player, *consumable);
        }
    }

    // Mirrors ItemStack.finishUsingItem + applyAfterUseComponentSideEffects
    // (ItemStack.java:326-348).
    ItemStack FinishUsing(Server::ServerPlayer& player, ItemStack& handStack) {
        const int countBeforeUsing = handStack.count;   // :327
        // Read BEFORE consuming. MC's consume leaves a count-0 stack that
        // still knows its item and components, so applyAfterUseComponent-
        // SideEffects can ask it for USE_REMAINDER afterwards; here a stack
        // that reaches zero is Clear()ed to air, and asking afterwards would
        // find nothing — the last potion would vanish instead of leaving its
        // glass bottle (and the last stew its bowl).
        const auto useRemainder = handStack.get(DataComponents::USE_REMAINDER);

        // Item.finishUsingItem (Item.java:221-224): CONSUMABLE → onConsume.
        if (auto consumable = handStack.get(DataComponents::CONSUMABLE)) {
            OnConsume(nullptr, player, handStack, *consumable);
        }
        ItemStack result = handStack;

        // applyAfterUseComponentSideEffects (ItemStack.java:332-348) →
        // UseRemainder.convertIntoRemainder (UseRemainder.java:12-26).
        if (const auto& remainder = useRemainder) {
            const bool hasInfiniteMaterials =
                player.getGameMode() == Server::GameMode::CREATIVE;
            if (!hasInfiniteMaterials && result.count < countBeforeUsing) {  // :13-16
                ItemStack remainderStack = remainder->convertInto;           // :18
                if (result.IsEmpty()) {
                    return remainderStack;                                   // :19-20
                }
                // :22 onExtraCreatedRemainder → Player.handleExtraItemsCreatedOnUse
                // (add to inventory, drop whatever doesn't fit).
                // Diff the inventory around the add so every slot the
                // remainder landed in gets broadcast via the dirty-slot
                // channel (the hand slot is handled by completeUsingItem).
                auto& inv = player.getInventory();
                std::array<ItemStack, Inventory::TOTAL_SIZE> before;
                for (int i = 0; i < Inventory::TOTAL_SIZE; ++i) before[i] = inv.GetSlot(i);
                const int leftover = inv.AddStack(remainderStack);
                for (int i = 0; i < Inventory::TOTAL_SIZE; ++i) {
                    const auto& after = inv.GetSlot(i);
                    if (after.itemId != before[i].itemId || after.count != before[i].count) {
                        player.markSlotDirty(i);
                    }
                }
                if (leftover > 0) {
                    // Inventory full — the remainder (an empty bucket, a bowl)
                    // goes on the ground at the player's feet rather than
                    // being destroyed.
                    ItemStack spill = remainderStack;
                    spill.count = leftover;
                    DropItemStackNear(DimensionFromRaw(player.getDimensionId()),
                                      glm::ivec3(glm::floor(player.getPosition())),
                                      spill);
                }
            }
        }
        return result;
    }

    ItemStack FinishUsingForLiving(LivingEntity& user, ItemStack& handStack) {
        EntityLevel* level = user.Level();
        if (!level || level->IsClientSide() || handStack.IsEmpty()) return handStack;
        const int countBeforeUsing = handStack.count;
        const auto useRemainder = handStack.get(DataComponents::USE_REMAINDER);
        if (auto consumable = handStack.get(DataComponents::CONSUMABLE)) {
            JavaRandom& r = level->Random();
            // Consumable.emitParticlesAndSounds(random, user, stack, 16).
            const float eatVolume = r.NextBool() ? 0.5f : 1.0f;
            const float eatPitch = static_cast<float>(r.Triangle(1.0, 0.2));
            const float drinkPitch = 0.9f + r.NextFloat() * 0.1f;   // Mth.randomBetween(0.9, 1.0)
            const bool drink = consumable->animation == ItemUseAnimation::DRINK;
            if (consumable->hasConsumeParticles) user.SpawnItemParticles(handStack, 16);
            if (!consumable->sound.empty()) {
                user.PlaySound(consumable->sound, drink ? 0.5f : eatVolume, drink ? drinkPitch : eatPitch);
            }
            // ConsumableListener components.
            if (auto food = handStack.get(DataComponents::FOOD)) {
                (void)food;
                // FoodProperties.onConsume: the consume sound for everyone
                // (NEUTRAL, triangle(1, 0.4)); the food data and the burp are
                // the player's.
                if (!consumable->sound.empty()) {
                    level->PlaySeededSound(nullptr, user.position, consumable->sound, SoundSource::Neutral,
                                           1.0f, static_cast<float>(r.Triangle(1.0, 0.4)), r.NextLong());
                }
            }
            if (auto potion = handStack.get(DataComponents::POTION_CONTENTS)) {
                potion->ApplyToLivingEntity(user, handStack.get(DataComponents::POTION_DURATION_SCALE).value_or(1.0f));
            }
            if (auto stew = handStack.get(DataComponents::SUSPICIOUS_STEW_EFFECTS)) {
                for (const auto& entry : stew->effects) user.AddEffect(entry.CreateEffectInstance());
            }
            if (auto amplifier = handStack.get(DataComponents::OMINOUS_BOTTLE_AMPLIFIER)) {
                user.AddEffect(MobEffectInstance(MobEffectId::BadOmen, DataComponents::kOminousBottleEffectDuration,
                                                 *amplifier, /*ambient=*/false, /*visible=*/false,
                                                 /*showIcon=*/true));
            }
            // onConsumeEffects.
            for (const auto& effect : consumable->onConsumeEffects) ApplyConsumeEffect(user, effect);
            user.GameEvent(drink ? GameEventId::Drink : GameEventId::Eat);
            // stack.consume(1, user) — a mob never has infinite materials.
            handStack.count -= 1;
            if (handStack.count <= 0) handStack.Clear();
        }
        ItemStack result = handStack;
        // UseRemainder.convertIntoRemainder: a stack used up becomes its
        // remainder; a partial one keeps its count (the mob's
        // handleExtraItemsCreatedOnUse drops nothing).
        if (useRemainder && result.count < countBeforeUsing && result.IsEmpty()) {
            return useRemainder->convertInto;
        }
        return result;
    }

} // namespace Game::ConsumableBehavior
