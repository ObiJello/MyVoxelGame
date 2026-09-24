// File: src/common/entity/alchemy/PotionItems.cpp
//
// The potion items' Item.Properties and behaviour — MC 26.3 Items.java rows
// for POTION / SPLASH_POTION / LINGERING_POTION / TIPPED_ARROW /
// SUSPICIOUS_STEW, plus:
//   PotionItem.useOn          a water bottle turns dirt into mud
//   ThrowablePotionItem.use   splash and lingering potions are thrown
//                             (SplashPotionItem / LingeringPotionItem.use)
// Drinking is not here: POTION's CONSUMABLE (Consumables.DEFAULT_DRINK) runs
// the ordinary consume lifecycle, and ConsumableBehavior::OnConsume applies
// POTION_CONTENTS as the ConsumableListener it is in MC.
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/data/DataComponents.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldMobSpawn.hpp"

#include <random>
#include <unordered_map>

namespace Game {

    namespace {

        // MC's `0.4F / (level.getRandom().nextFloat() * 0.4F + 0.8F)` throw pitch.
        float ThrowPitch(ILevelWrite& level) {
            JavaRandom* r = level.Random();
            return 0.4f / ((r ? r->NextFloat() : 0.5f) * 0.4f + 0.8f);
        }

        // MC PotionItem.useOn — Potions.WATER on a #convertable_to_mud block
        // (dirt, coarse dirt, rooted dirt), any face but the bottom: the block
        // becomes mud and the bottle comes back empty. Runs on both sides
        // like every useOn here — the mud is predicted and the server's write
        // confirms it.
        UseResult UseOn_Potion(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world || !ctx.player) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID block = ctx.world->GetBlock(pos.x, pos.y, pos.z);
            const bool convertible = block == BlockID::Dirt || block == BlockID::CoarseDirt ||
                                     block == BlockID::RootedDirt;
            const PotionContents contents = GetPotionContents(stack);
            if (ctx.hitResult.face == 0 /* DOWN */ || !convertible ||
                !contents.Is(PotionId::Water)) {
                return UseResult::Pass;
            }
            // PotionItem.useOn:43 — playSound(null, pos, GENERIC_SPLASH, BLOCKS):
            // the server's, heard by everyone including the user.
            ctx.world->PlaySound(nullptr, pos, SoundEvents::GENERIC_SPLASH, SoundSource::Blocks, 1.0f, 1.0f);
            ctx.player->CreateFilledResult(stack, ItemStack(Items::GlassBottle, 1));
            ctx.player->markSlotDirty(ctx.player->handSlotIndex(ctx.hand));
            // The five SPLASH particles have no system here.
            // PotionItem.useOn:53 — BOTTLE_EMPTY, the same way.
            ctx.world->PlaySound(nullptr, pos, SoundEvents::BOTTLE_EMPTY, SoundSource::Blocks, 1.0f, 1.0f);
            // GameEvent.FLUID_PLACE — no game-event system.
            ctx.world->SetBlock(pos.x, pos.y, pos.z, BlockID::Mud, World::UpdateFlags::All);
            return UseResult::Success;
        }

        // MC SplashPotionItem.use / LingeringPotionItem.use →
        // ThrowablePotionItem.use: the throw sound, the projectile (server
        // side only — the client swings and waits for the entity on the
        // wire), and itemStack.consume(1, player).
        UseResult Use_ThrowPotion(ILevelWrite* world, IUsePlayer* player,
                                  uint32_t /*hand*/, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;

            const bool lingering = stack.itemId == Items::LingeringPotion;
            // SplashPotionItem.use:23 (PLAYERS) / LingeringPotionItem.use:23
            // (NEUTRAL) — playSound(null, player, …, 0.5, throw pitch).
            world->PlaySound(nullptr, player->getPosition(),
                             lingering ? SoundEvents::LINGERING_POTION_THROW : SoundEvents::SPLASH_POTION_THROW,
                             lingering ? SoundSource::Neutral : SoundSource::Players, 0.5f, ThrowPitch(*world));

            if (!ThrowPotion(player->getDimensionId(), *player, stack)) {
                // No server-side owner to throw from — keep the potion.
                return UseResult::Fail;
            }
            // ItemStack.consume: creative keeps it (hasInfiniteMaterials).
            if (!player->isCreative()) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
            return UseResult::Success;
        }

    } // namespace

    // Called from ItemRegistry_RegisterBehaviors (ItemBehaviors.cpp).
    void ItemRegistry_RegisterPotionItems(std::unordered_map<ItemID, Item>& pureItems) {
        using DataComponents::CONSUMABLE;
        using DataComponents::POTION_CONTENTS;
        using DataComponents::POTION_DURATION_SCALE;
        using DataComponents::SUSPICIOUS_STEW_EFFECTS;
        using DataComponents::USE_REMAINDER;

        auto find = [&](ItemID id) -> Item* {
            auto it = pureItems.find(id);
            return it != pureItems.end() ? &it->second : nullptr;
        };

        // Items.java: POTION — stacksTo(1), POTION_CONTENTS EMPTY,
        // CONSUMABLE Consumables.DEFAULT_DRINK, usingConvertsTo(GLASS_BOTTLE).
        if (Item* potion = find(Items::Potion)) {
            potion->maxStackSize = 1;
            potion->defaultComponents.set(POTION_CONTENTS, PotionContents{});
            Consumable drink;                          // Consumables.defaultDrink()
            drink.consumeSeconds      = 1.6f;          // → 32 ticks
            drink.animation           = ItemUseAnimation::DRINK;
            drink.sound               = "entity.generic.drink";
            drink.hasConsumeParticles = false;
            potion->defaultComponents.set(CONSUMABLE, drink);
            potion->defaultComponents.set(USE_REMAINDER,
                                          UseRemainder{ItemStack(Items::GlassBottle, 1)});
            potion->useOn = &UseOn_Potion;
        }
        // SPLASH_POTION — stacksTo(1), POTION_CONTENTS EMPTY.
        if (Item* splash = find(Items::SplashPotion)) {
            splash->maxStackSize = 1;
            splash->defaultComponents.set(POTION_CONTENTS, PotionContents{});
            splash->use   = &Use_ThrowPotion;
            splash->useOn = &UseOn_Potion;   // ThrowablePotionItem extends PotionItem
        }
        // LINGERING_POTION — stacksTo(1), POTION_CONTENTS EMPTY,
        // POTION_DURATION_SCALE 0.25.
        if (Item* lingering = find(Items::LingeringPotion)) {
            lingering->maxStackSize = 1;
            lingering->defaultComponents.set(POTION_CONTENTS, PotionContents{});
            lingering->defaultComponents.set(POTION_DURATION_SCALE, 0.25f);
            lingering->use   = &Use_ThrowPotion;
            lingering->useOn = &UseOn_Potion;
        }
        // TIPPED_ARROW — POTION_CONTENTS EMPTY, POTION_DURATION_SCALE 0.125
        // (stacks to 64 like any arrow).
        if (Item* tipped = find(Items::TippedArrow)) {
            tipped->defaultComponents.set(POTION_CONTENTS, PotionContents{});
            tipped->defaultComponents.set(POTION_DURATION_SCALE, 0.125f);
        }
        // SUSPICIOUS_STEW — SUSPICIOUS_STEW_EFFECTS EMPTY (its FOOD /
        // CONSUMABLE / bowl remainder are FoodDefs.cpp's).
        if (Item* stew = find(Items::SuspiciousStew)) {
            stew->defaultComponents.set(SUSPICIOUS_STEW_EFFECTS, SuspiciousStewEffects{});
        }
    }

} // namespace Game
