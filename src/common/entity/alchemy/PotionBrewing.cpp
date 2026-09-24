// File: src/common/entity/alchemy/PotionBrewing.cpp
//
// See PotionBrewing.hpp. The table is VanillaBrewingProvider.buildRecipes()
// (addContainers, addContainerTransformations, buildMixes,
// buildTransformations) replayed verbatim.
#include "common/entity/alchemy/PotionBrewing.hpp"

#include "common/entity/alchemy/Potions.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/data/DataComponents.hpp"

#include <algorithm>
#include <set>
#include <vector>

namespace Game {

    namespace {

        struct BrewingRecipe {
            ItemID   inputItem;
            PotionId inputPotion;
            ItemID   reagent;
            ItemID   outputItem;
            PotionId outputPotion;
        };

        struct BrewingTable {
            std::vector<BrewingRecipe> recipes;
            std::vector<ItemID>        reagents;   // BREWING_REAGENTS, sorted
            std::vector<ItemID>        inputs;     // BREWING_INPUTS, sorted
        };

        const BrewingTable& Table() {
            static const BrewingTable table = [] {
                BrewingTable t;
                using P = PotionId;

                // addContainers — Items.LINGERING_POTION, POTION, SPLASH_POTION.
                const std::vector<ItemID> containers = {
                    Items::LingeringPotion, Items::Potion, Items::SplashPotion,
                };
                // addContainerTransformations.
                struct Transform { ItemID container, reagent, output; };
                const std::vector<Transform> transforms = {
                    {Items::Potion,       Items::Gunpowder,    Items::SplashPotion},
                    {Items::SplashPotion, Items::DragonBreath, Items::LingeringPotion},
                };

                // BrewingProvider.potions is a HashSet — its iteration order
                // decides nothing, since no two transformation recipes share
                // an input — kept here in first-mentioned order.
                std::vector<P> potions;
                auto remember = [&](P p) {
                    if (std::find(potions.begin(), potions.end(), p) == potions.end()) {
                        potions.push_back(p);
                    }
                };

                // BrewingProvider.buildMix: one recipe per container.
                auto mix = [&](P input, ItemID reagent, P output) {
                    for (ItemID container : containers) {
                        t.recipes.push_back({container, input, reagent, container, output});
                    }
                    remember(input);
                    remember(output);
                };
                // BrewingProvider.buildStartMix.
                auto startMix = [&](ItemID reagent, P output) {
                    mix(P::Water, reagent, P::Mundane);
                    mix(P::Awkward, reagent, output);
                };

                const ItemID stone     = ItemRegistry::FromBlock(BlockID::Stone);
                const ItemID slime     = ItemRegistry::FromBlock(BlockID::SlimeBlock);
                const ItemID cobweb    = ItemRegistry::FromBlock(BlockID::Cobweb);

                // VanillaBrewingProvider.buildMixes, verbatim order.
                mix(P::Water, Items::GlowstoneDust, P::Thick);
                mix(P::Water, Items::Redstone, P::Mundane);
                mix(P::Water, Items::NetherWart, P::Awkward);
                startMix(Items::BreezeRod, P::WindCharged);
                startMix(slime, P::Oozing);
                startMix(stone, P::Infested);
                startMix(cobweb, P::Weaving);
                mix(P::Awkward, Items::GoldenCarrot, P::NightVision);
                mix(P::NightVision, Items::Redstone, P::LongNightVision);
                mix(P::NightVision, Items::FermentedSpiderEye, P::Invisibility);
                mix(P::LongNightVision, Items::FermentedSpiderEye, P::LongInvisibility);
                mix(P::Invisibility, Items::Redstone, P::LongInvisibility);
                startMix(Items::MagmaCream, P::FireResistance);
                mix(P::FireResistance, Items::Redstone, P::LongFireResistance);
                startMix(Items::RabbitFoot, P::Leaping);
                mix(P::Leaping, Items::Redstone, P::LongLeaping);
                mix(P::Leaping, Items::GlowstoneDust, P::StrongLeaping);
                mix(P::Leaping, Items::FermentedSpiderEye, P::Slowness);
                mix(P::LongLeaping, Items::FermentedSpiderEye, P::LongSlowness);
                mix(P::Slowness, Items::Redstone, P::LongSlowness);
                mix(P::Slowness, Items::GlowstoneDust, P::StrongSlowness);
                mix(P::Awkward, Items::TurtleHelmet, P::TurtleMaster);
                mix(P::TurtleMaster, Items::Redstone, P::LongTurtleMaster);
                mix(P::TurtleMaster, Items::GlowstoneDust, P::StrongTurtleMaster);
                mix(P::Swiftness, Items::FermentedSpiderEye, P::Slowness);
                mix(P::LongSwiftness, Items::FermentedSpiderEye, P::LongSlowness);
                startMix(Items::Sugar, P::Swiftness);
                mix(P::Swiftness, Items::Redstone, P::LongSwiftness);
                mix(P::Swiftness, Items::GlowstoneDust, P::StrongSwiftness);
                mix(P::Awkward, Items::Pufferfish, P::WaterBreathing);
                mix(P::WaterBreathing, Items::Redstone, P::LongWaterBreathing);
                startMix(Items::GlisteringMelonSlice, P::Healing);
                mix(P::Healing, Items::GlowstoneDust, P::StrongHealing);
                mix(P::Healing, Items::FermentedSpiderEye, P::Harming);
                mix(P::StrongHealing, Items::FermentedSpiderEye, P::StrongHarming);
                mix(P::Harming, Items::GlowstoneDust, P::StrongHarming);
                mix(P::Poison, Items::FermentedSpiderEye, P::Harming);
                mix(P::LongPoison, Items::FermentedSpiderEye, P::Harming);
                mix(P::StrongPoison, Items::FermentedSpiderEye, P::StrongHarming);
                startMix(Items::SpiderEye, P::Poison);
                mix(P::Poison, Items::Redstone, P::LongPoison);
                mix(P::Poison, Items::GlowstoneDust, P::StrongPoison);
                startMix(Items::GhastTear, P::Regeneration);
                mix(P::Regeneration, Items::Redstone, P::LongRegeneration);
                mix(P::Regeneration, Items::GlowstoneDust, P::StrongRegeneration);
                startMix(Items::BlazePowder, P::Strength);
                mix(P::Strength, Items::Redstone, P::LongStrength);
                mix(P::Strength, Items::GlowstoneDust, P::StrongStrength);
                mix(P::Water, Items::FermentedSpiderEye, P::Weakness);
                mix(P::Weakness, Items::Redstone, P::LongWeakness);
                mix(P::Awkward, Items::PhantomMembrane, P::SlowFalling);
                mix(P::SlowFalling, Items::Redstone, P::LongSlowFalling);

                // buildTransformations: every (transformation, potion) pair,
                // the output container carrying the SAME potion.
                for (const Transform& tr : transforms) {
                    for (P potion : potions) {
                        t.recipes.push_back({tr.container, potion, tr.reagent, tr.output, potion});
                    }
                }

                // The two RecipePropertySets the stand consults.
                std::set<ItemID> reagents, inputs;
                for (const BrewingRecipe& r : t.recipes) {
                    reagents.insert(r.reagent);
                    inputs.insert(r.inputItem);
                }
                t.reagents.assign(reagents.begin(), reagents.end());
                t.inputs.assign(inputs.begin(), inputs.end());
                return t;
            }();
            return table;
        }

        bool Contains(const std::vector<ItemID>& sorted, ItemID id) {
            return std::binary_search(sorted.begin(), sorted.end(), id);
        }

    } // namespace

    std::optional<ItemStack> FindBrewingResult(const ItemStack& input, const ItemStack& reagent) {
        if (input.IsEmpty() || reagent.IsEmpty()) return std::nullopt;
        // PotionsPredicate.ofPotion(p).matches: the stack's POTION_CONTENTS
        // names exactly that potion — custom effects on top do not matter.
        const auto contents = input.get(DataComponents::POTION_CONTENTS);
        if (!contents || !contents->potion) return std::nullopt;
        for (const BrewingRecipe& r : Table().recipes) {
            if (r.inputItem != input.itemId || r.reagent != reagent.itemId) continue;
            if (*contents->potion != r.inputPotion) continue;
            // BrewingRecipe.assemble → output.create(): the container with
            // PotionContents(outPotion), count 1.
            return CreatePotionItemStack(r.outputItem, r.outputPotion);
        }
        return std::nullopt;
    }

    bool IsBrewingReagent(const ItemStack& stack) {
        return !stack.IsEmpty() && Contains(Table().reagents, stack.itemId);
    }

    bool IsBrewingPotionInput(const ItemStack& stack) {
        if (stack.IsEmpty()) return false;
        // BREWING_INPUTS, or #brewing_potion_inputs (potion, splash_potion,
        // lingering_potion, glass_bottle — VanillaItemTagsProvider).
        return Contains(Table().inputs, stack.itemId) || stack.itemId == Items::Potion ||
               stack.itemId == Items::SplashPotion || stack.itemId == Items::LingeringPotion ||
               stack.itemId == Items::GlassBottle;
    }

    int GetBrewingFuelUses(const ItemStack& stack) {
        // ContextIntProviders.BREWING_DEFAULT_USES — 20.
        return (!stack.IsEmpty() && stack.itemId == Items::BlazePowder) ? 20 : 0;
    }

    float GetBrewingFuelSpeedMultiplier(const ItemStack& stack) {
        // ContextFloatProviders.BREWING_DEFAULT_SPEED_MULTIPLIER — 1.0.
        (void)stack;
        return 1.0f;
    }

} // namespace Game
