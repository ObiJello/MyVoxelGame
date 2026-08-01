// File: src/common/entity/FoodDefs.cpp
//
// Per-item FOOD + CONSUMABLE (+ USE_REMAINDER) defaults for every vanilla
// food. Values are verbatim from:
//   • world/food/Foods.java            (nutrition + saturationModifier rows)
//   • world/item/component/Consumables.java (consume seconds / animation /
//     sound / on-consume effects)
//   • Items.java `.usingConvertsTo(...)` / `.stacksTo(...)` rows (stew bowls,
//     honey → glass bottle, milk → bucket, stack sizes)
//
// Saturation is stored FINAL: FoodProperties.Builder.build converts the
// modifier via FoodConstants.saturationByModifier = nutrition * modifier * 2
// (FoodProperties.java:60-62, FoodConstants.java:30-32) — the Food() helper
// below does the same conversion so the table reads like Foods.java.
//
// Status-effect payloads are carried as data (logged on consume) — the
// effect system doesn't exist yet, but the food definitions are complete.
#include "Item.hpp"
#include "GeneratedItemList.hpp"
#include "../data/DataComponents.hpp"
#include "../core/Log.hpp"

#include <unordered_map>

namespace Game {

    void ItemRegistry_RegisterFoods(std::unordered_map<ItemID, Item>& pureItems) {
        using DataComponents::CONSUMABLE;
        using DataComponents::FOOD;
        using DataComponents::USE_REMAINDER;

        // FoodProperties.Builder equivalent — nutrition + MODIFIER in,
        // final saturation out (FoodConstants.saturationByModifier).
        auto Food = [](int nutrition, float saturationModifier,
                       bool alwaysEdible = false) {
            return FoodProperties{
                nutrition,
                static_cast<float>(nutrition) * saturationModifier * 2.0f,
                alwaysEdible,
            };
        };

        // Consumables.defaultFood() — Consumables.java:29-31.
        auto DefaultFood = []() {
            Consumable c;
            c.consumeSeconds      = 1.6f;
            c.animation           = ItemUseAnimation::EAT;
            c.sound               = "entity.generic.eat";
            c.hasConsumeParticles = true;
            return c;
        };
        // Consumables.defaultDrink() — Consumables.java:33-35.
        auto DefaultDrink = []() {
            Consumable c;
            c.consumeSeconds      = 1.6f;
            c.animation           = ItemUseAnimation::DRINK;
            c.sound               = "entity.generic.drink";
            c.hasConsumeParticles = false;
            return c;
        };
        auto WithEffect = [](Consumable c, ConsumeEffect::Type type,
                             const char* payload) {
            c.onConsumeEffects.push_back(ConsumeEffect{type, payload});
            return c;
        };

        // Attach components to one item. maxStack ≤ 0 → leave unchanged.
        auto Set = [&](ItemID id, const FoodProperties* food,
                       const Consumable& consumable,
                       ItemID remainderItem = Items::Air, int maxStack = -1) {
            auto it = pureItems.find(id);
            if (it == pureItems.end()) return;
            if (food) it->second.defaultComponents.set(FOOD, *food);
            it->second.defaultComponents.set(CONSUMABLE, consumable);
            if (remainderItem != Items::Air) {
                it->second.defaultComponents.set(
                    USE_REMAINDER, UseRemainder{ItemStack(remainderItem, 1)});
            }
            if (maxStack > 0) it->second.maxStackSize = maxStack;
        };
        auto SetF = [&](ItemID id, FoodProperties food) {
            Set(id, &food, DefaultFood());
        };

        using ET = ConsumeEffect::Type;

        // ── Plain foods (Foods.java row order) ──────────────────────────────
        SetF(Items::Apple,          Food(4, 0.3f));   // Foods.java:4
        SetF(Items::BakedPotato,    Food(5, 0.6f));   // :5
        SetF(Items::Beef,           Food(3, 0.3f));   // :6
        SetF(Items::Beetroot,       Food(1, 0.6f));   // :7
        SetF(Items::Bread,          Food(5, 0.6f));   // :9
        SetF(Items::Carrot,         Food(3, 0.6f));   // :10
        SetF(Items::Cod,            Food(2, 0.1f));   // :13
        SetF(Items::CookedBeef,     Food(8, 0.8f));   // :14
        SetF(Items::CookedChicken,  Food(6, 0.6f));   // :15
        SetF(Items::CookedCod,      Food(5, 0.6f));   // :16
        SetF(Items::CookedMutton,   Food(6, 0.8f));   // :17
        SetF(Items::CookedPorkchop, Food(8, 0.8f));   // :18
        SetF(Items::CookedRabbit,   Food(5, 0.6f));   // :19
        SetF(Items::CookedSalmon,   Food(6, 0.8f));   // :20
        SetF(Items::Cookie,         Food(2, 0.1f));   // :21
        SetF(Items::GoldenCarrot,   Food(6, 1.2f));   // :25
        SetF(Items::MelonSlice,     Food(2, 0.3f));   // :27
        SetF(Items::Mutton,         Food(2, 0.3f));   // :29
        SetF(Items::Porkchop,       Food(3, 0.3f));   // :31
        SetF(Items::Potato,         Food(1, 0.3f));   // :32
        SetF(Items::PumpkinPie,     Food(8, 0.3f));   // :34
        SetF(Items::Rabbit,         Food(3, 0.3f));   // :35
        SetF(Items::Salmon,         Food(2, 0.1f));   // :38
        SetF(Items::SweetBerries,   Food(2, 0.1f));   // :41
        SetF(Items::GlowBerries,    Food(2, 0.1f));   // :42
        SetF(Items::TropicalFish,   Food(1, 0.1f));   // :43

        // ── Foods with special Consumables rows (Consumables.java:37-50) ────
        {   // DRIED_KELP — 0.8 s (:40)
            auto c = DefaultFood(); c.consumeSeconds = 0.8f;
            auto f = Food(1, 0.3f);                    // Foods.java:22
            Set(Items::DriedKelp, &f, c);
        }
        {   // CHICKEN — 30% hunger 0:30 (:41)
            auto f = Food(2, 0.3f);                    // Foods.java:11
            Set(Items::Chicken, &f,
                WithEffect(DefaultFood(), ET::ApplyStatusEffects,
                           "hunger 600t amp0 chance0.3"));
        }
        {   // GOLDEN_APPLE — regen II 0:05 + absorption 2:00 (:43)
            auto f = Food(4, 1.2f, /*alwaysEdible=*/true);  // Foods.java:24
            Set(Items::GoldenApple, &f,
                WithEffect(DefaultFood(), ET::ApplyStatusEffects,
                           "regeneration 100t amp1; absorption 2400t amp0"));
        }
        {   // ENCHANTED_GOLDEN_APPLE — regen/resist/fire-resist/absorption (:42)
            auto f = Food(4, 1.2f, /*alwaysEdible=*/true);  // Foods.java:23
            Set(Items::EnchantedGoldenApple, &f,
                WithEffect(DefaultFood(), ET::ApplyStatusEffects,
                           "regeneration 400t amp1; resistance 6000t amp0; "
                           "fire_resistance 6000t amp0; absorption 2400t amp3"));
        }
        {   // POISONOUS_POTATO — 60% poison 0:05 (:44)
            auto f = Food(2, 0.3f);                    // Foods.java:30
            Set(Items::PoisonousPotato, &f,
                WithEffect(DefaultFood(), ET::ApplyStatusEffects,
                           "poison 100t amp0 chance0.6"));
        }
        {   // PUFFERFISH — poison II 1:00 + hunger III 0:15 + nausea 0:15 (:45)
            auto f = Food(1, 0.1f);                    // Foods.java:33
            Set(Items::Pufferfish, &f,
                WithEffect(DefaultFood(), ET::ApplyStatusEffects,
                           "poison 1200t amp1; hunger 300t amp2; nausea 300t amp0"));
        }
        {   // ROTTEN_FLESH — 80% hunger 0:30 (:46)
            auto f = Food(4, 0.1f);                    // Foods.java:37
            Set(Items::RottenFlesh, &f,
                WithEffect(DefaultFood(), ET::ApplyStatusEffects,
                           "hunger 600t amp0 chance0.8"));
        }
        {   // SPIDER_EYE — poison 0:05 (:47)
            auto f = Food(2, 0.8f);                    // Foods.java:39
            Set(Items::SpiderEye, &f,
                WithEffect(DefaultFood(), ET::ApplyStatusEffects,
                           "poison 100t amp0"));
        }
        {   // CHORUS_FRUIT — random teleport (:49)
            auto f = Food(4, 0.3f, /*alwaysEdible=*/true);  // Foods.java:12
            Set(Items::ChorusFruit, &f,
                WithEffect(DefaultFood(), ET::TeleportRandomly, "diameter16"));
        }

        // ── Stews / soups — usingConvertsTo(BOWL) + stacksTo(1) (Items.java) ─
        {
            auto f = Food(6, 0.6f);                    // stew(6), Foods.java:28
            Set(Items::MushroomStew, &f, DefaultFood(), Items::Bowl, 1);
        }
        {
            auto f = Food(6, 0.6f);                    // stew(6), Foods.java:8
            Set(Items::BeetrootSoup, &f, DefaultFood(), Items::Bowl, 1);
        }
        {
            auto f = Food(10, 0.6f);                   // stew(10), Foods.java:36
            Set(Items::RabbitStew, &f, DefaultFood(), Items::Bowl, 1);
        }
        {   // SUSPICIOUS_STEW — alwaysEdible; per-stack effects come from the
            // SUSPICIOUS_STEW_EFFECTS component in MC (not modelled).
            auto f = Food(6, 0.6f, /*alwaysEdible=*/true);  // Foods.java:40
            Set(Items::SuspiciousStew, &f, DefaultFood(), Items::Bowl, 1);
        }

        // ── Drinks ──────────────────────────────────────────────────────────
        {   // HONEY_BOTTLE — 2.0 s drink, removes poison, → glass bottle,
            // stacksTo(16) (Consumables.java:38 + Items.java honey_bottle row)
            auto c = DefaultDrink();
            c.consumeSeconds = 2.0f;
            c.sound          = "item.honey_bottle.drink";
            c = WithEffect(std::move(c), ET::RemoveStatusEffects, "poison");
            auto f = Food(6, 0.1f, /*alwaysEdible=*/true);  // Foods.java:26
            Set(Items::HoneyBottle, &f, c, Items::GlassBottle, 16);
        }
        {   // MILK_BUCKET — drink, clears all effects, → bucket, stacksTo(1)
            // (Consumables.java:48 + Items.java milk_bucket row). No FOOD.
            auto c = WithEffect(DefaultDrink(), ET::ClearAllStatusEffects, "");
            Set(Items::MilkBucket, nullptr, c, Items::Bucket, 1);
        }

        Log::Info("[ItemRegistry] Registered FOOD/CONSUMABLE defaults on 42 food items");
    }

} // namespace Game
