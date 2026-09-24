// File: src/common/entity/ConsumableBehavior.cpp
// See header. MC citations per function. Sounds play through the player's
// level (Level.playSound); particles and game events are still stubs.
#include "ConsumableBehavior.hpp"

#include "../core/Log.hpp"
#include "Inventory.hpp"
#include "../world/level/WorldDrops.hpp"
#include "server/player/ServerPlayer.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/World.hpp"

#include <array>
#include <cstdlib>
#include <sstream>
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

        // The FoodDefs payload for ApplyStatusEffects: "name Nt ampM
        // [chanceP]" entries separated by ';' — the ApplyStatusEffects
        // ConsumeEffect record's (effects, probability). The chance, when
        // present, is the record's single probability.
        struct ParsedApply {
            std::vector<MobEffectInstance> effects;
            float probability = 1.0f;
        };

        ParsedApply ParseApplyPayload(const std::string& payload) {
            ParsedApply out;
            size_t start = 0;
            while (start <= payload.size()) {
                size_t end = payload.find(';', start);
                if (end == std::string::npos) end = payload.size();
                std::istringstream entry(payload.substr(start, end - start));
                std::string name, word;
                if (entry >> name) {
                    int duration = 0, amplifier = 0;
                    while (entry >> word) {
                        if (word.size() > 1 && word.back() == 't') {
                            duration = std::atoi(word.substr(0, word.size() - 1).c_str());
                        } else if (word.rfind("amp", 0) == 0) {
                            amplifier = std::atoi(word.substr(3).c_str());
                        } else if (word.rfind("chance", 0) == 0) {
                            out.probability = static_cast<float>(std::atof(word.substr(6).c_str()));
                        }
                    }
                    MobEffectId id;
                    if (ParseEffectId(name, id)) out.effects.emplace_back(id, duration, amplifier);
                    else Log::Warning("[Consume] unknown effect '%s' in payload", name.c_str());
                }
                start = end + 1;
            }
            return out;
        }

        void ApplyConsumeEffect(Server::ServerPlayer& player, const ConsumeEffect& effect) {
            switch (effect.type) {
                case ConsumeEffect::Type::ApplyStatusEffects: {
                    // ApplyStatusEffectsConsumeEffect.apply:
                    // `user.getRandom().nextFloat() >= probability` skips the
                    // whole list; otherwise each effect is added in turn.
                    const ParsedApply parsed = ParseApplyPayload(effect.payload);
                    LivingEntity* user = player.effectEntity();
                    if (!user) break;
                    if (EntityLevel* level = user->Level()) {
                        if (level->Random().NextFloat() >= parsed.probability) break;
                    }
                    for (const MobEffectInstance& e : parsed.effects) {
                        player.addEffect(MobEffectInstance(e));
                    }
                    break;
                }
                case ConsumeEffect::Type::RemoveStatusEffects: {
                    // RemoveStatusEffectsConsumeEffect: every listed effect.
                    std::istringstream names(effect.payload);
                    std::string name;
                    while (names >> name) {
                        MobEffectId id;
                        if (ParseEffectId(name, id)) player.removeEffect(id);
                    }
                    break;
                }
                case ConsumeEffect::Type::ClearAllStatusEffects:
                    // ClearAllStatusEffectsConsumeEffect — milk.
                    player.removeAllEffects();
                    break;
                case ConsumeEffect::Type::PlaySound: {
                    // PlaySoundConsumeEffect.apply: level.playSound(null, user
                    // x/y/z, sound, user.getSoundSource(), 1, 1).
                    if (World* world = player.soundWorld(); world && !effect.payload.empty()) {
                        world->PlaySound(nullptr, player.getPosition(), effect.payload,
                                         SoundSource::Players, 1.0f, 1.0f);
                    }
                    break;
                }
                default:
                    // TeleportRandomly: no random-teleport system here yet.
                    Log::Debug("[Consume] effect type=%u payload='%s' — no system to apply it",
                               static_cast<unsigned>(effect.type), effect.payload.c_str());
                    break;
            }
        }
    } // namespace

    // Mirrors Consumable.onConsume — Consumable.java:54-70.
    void OnConsume(World* world, Server::ServerPlayer& player,
                   ItemStack& stack, const Consumable& consumable) {
        (void)world;

        // :56 emitParticlesAndSounds(random, user, stack, 16 particles). The
        // particle half (16 item particles at the mouth) is still TODO.
        EmitConsumeSound(player, consumable);

        // :58-59 awardStat + CONSUME_ITEM criteria — no stats/advancements.

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

        // :63-65 onConsumeEffects.forEach(apply) — server side, which this
        // always is. The status-effect kinds run through the player's
        // LivingEntity (ServerPlayer::addEffect → PlayerEntityView), exactly
        // MC's user.addEffect / removeEffect / removeAllEffects.
        for (const auto& effect : consumable.onConsumeEffects) {
            ApplyConsumeEffect(player, effect);
        }

        // :67 gameEvent EAT/DRINK — game-event system TODO.

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
            // :1063 emitParticlesAndSounds(…, 5 particles) — the sound half.
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

} // namespace Game::ConsumableBehavior
