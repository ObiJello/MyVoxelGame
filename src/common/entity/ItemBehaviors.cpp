// File: src/common/entity/ItemBehaviors.cpp
//
// Per-item Item.useOn / Item.use implementations. Mirrors MC's pattern of
// one small subclass per Item that needs custom behaviour. Each function in
// this file targets a specific MC source file (cited above the body) and
// keeps the same control flow — every branch MC takes, we take. Only
// system-dependent calls (sound playback, game events, durability damage,
// criteria triggers) are stubbed with TODO comments because the underlying
// systems aren't built yet; the LOGIC and MAPS are MC-faithful so wiring
// those systems in later is a small follow-up.
//
// Each callback returns one of:
//   UseResult::Success                 — action handled, swing arm
//   UseResult::Consume                 — action handled, no arm swing
//   UseResult::Fail                    — explicit reject, stop dispatch
//   UseResult::Pass                    — no opinion, fall through
//   UseResult::TryEmptyHandInteraction — defer to block.useWithoutItem
// PlayerSession::HandleUseItemOn checks `ConsumesAction(r)` to decide
// whether to stop the dispatch chain or fall through.

#include "Item.hpp"
#include "GeneratedItemList.hpp"
#include "SpawnEggs.hpp"
#include "../world/level/WorldMobSpawn.hpp"
#include "mobs/Animals.hpp"
#include "../data/DataComponents.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../world/block/BlockPlacement.hpp"
#include "../world/level/World.hpp"
#include "../world/level/WorldDrops.hpp"
#include "../core/JavaRandom.hpp"
#include "../core/Mth.hpp"
#include "../core/Log.hpp"
#include "IUsePlayer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_map>

namespace Game {

    // Implemented in FoodDefs.cpp — FOOD/CONSUMABLE/USE_REMAINDER defaults
    // for every vanilla food (Foods.java + Consumables.java values).
    void ItemRegistry_RegisterFoods(std::unordered_map<ItemID, Item>& pureItems);

    // Implemented in EquipmentBehavior.cpp — EQUIPPABLE on armor/elytra +
    // shield BLOCKS_ATTACKS (Items.java + ArmorMaterials.java values).
    void ItemRegistry_RegisterEquipment(std::unordered_map<ItemID, Item>& pureItems);

    // Implemented in BundleBehavior.cpp — bundle click overrides +
    // canFitInsideContainerItems + crafting remainders.
    void ItemRegistry_RegisterBundles(std::unordered_map<ItemID, Item>& pureItems);

    namespace {

        // ── Common helpers ──────────────────────────────────────────────────

        // Mirrors MC `Level.playSound(player, pos, soundEvent, source, volume,
        // pitch)` (Level.java:1013). We have no sound system yet — log instead
        // so the trigger sites are visible during testing. Replace with the
        // real `SoundEventS2CPacket` call when sounds land.
        void PlaySound(const char* eventName, const glm::ivec3& pos) {
            (void)pos;
            // TODO(sounds): broadcast a SoundEventS2CPacket(eventName, pos, vol, pitch)
            //               from `m_serverWorldInstance` to all players in range.
            Log::Debug("[Sound] %s at (%d,%d,%d) — TODO: wire sound system",
                       eventName, pos.x, pos.y, pos.z);
        }

        // Mirrors MC `Level.gameEvent(GameEvent, BlockPos, Context)`
        // (Level.java:1129). Sculk sensors / wardens listen on these. We don't
        // simulate them yet; this is a no-op marker that captures the intent.
        void GameEventEmit(const char* eventName, const glm::ivec3& pos) {
            (void)eventName; (void)pos;
            // TODO(game-events): once GameEvent system exists, broadcast here.
        }

        // Mirrors MC `ItemStack.hurtAndBreak(amount, owner, slot)`
        // (ItemStack.java:728). Reads the DAMAGE component, increments it, and
        // breaks the item if it would exceed the MAX_DAMAGE component.
        // We have the DataComponentMap infra (used for enchanted_book) but no
        // DAMAGE / MAX_DAMAGE components registered yet — those plus the
        // `breakItem` flow (sound, particles, slot empty) are a follow-up PR.
        void HurtAndBreak(ItemStack& stack, int /*amount*/, uint32_t /*hand*/) {
            (void)stack;
            // TODO(durability): once DataComponents::DAMAGE + MAX_DAMAGE land:
            //   auto dmg = stack.get(DataComponents::DAMAGE).value_or(0);
            //   auto max = stack.get(DataComponents::MAX_DAMAGE).value_or(0);
            //   if (max > 0 && ++dmg >= max) {
            //       playBreakSound(); spawnBreakParticles(); stack.Clear();
            //       triggerItemBroken(player, slot);
            //   } else if (max > 0) {
            //       stack.components.set(DataComponents::DAMAGE, dmg);
            //   }
        }

        // MC `Block.popResourceFromFace(level, pos, face, itemStack)` —
        // spawns an item entity at the clicked face with a small velocity
        // outward. Used by HoeItem when tilling rooted_dirt (drops a
        // hanging_roots item).
        void PopResourceFromFace(ILevelWrite* world, const glm::ivec3& pos,
                                 int face, BlockID dropId) {
            (void)world;
            DropItemStackFromFace(pos, face, ItemStack(dropId, 1));
        }

        // MC `level.isClientSide()` is `false` server-side. All our useOn
        // callbacks run on the SERVER (PlayerSession lives there), so the
        // `if (!level.isClientSide())` guards in MC's source map to "always
        // run" here — kept as comments next to each guarded block for clarity.

        // ── BaseFireBlock.canBePlacedAt (BaseFireBlock.java:172) ────────────
        // Target must be air AND `getState(level, pos).canSurvive(level, pos)`
        // must hold (or it's in a portal frame). FireBlock.canSurvive
        // (FireBlock.java:108) returns true when the block BELOW has a sturdy
        // upward face OR there's a flammable neighbour (`isValidFireLocation`).
        // We approximate the sturdy-face check via "block below is opaque",
        // which covers every vanilla solid block.
        bool CanFireBePlacedAt(ILevelWrite* world, const glm::ivec3& pos) {
            if (!world) return false;
            if (!world->IsValidPosition(pos.x, pos.y, pos.z)) return false;
            if (world->GetBlock(pos.x, pos.y, pos.z) != BlockID::Air) return false;
            // Block below must be solid for fire to survive on its top face.
            // (Skipping the soul-fire variant + isValidFireLocation flammable
            // neighbour check — both produce the same Boolean for solid floors,
            // which is the overwhelming common case. Refine when we add fire
            // spread behaviour and SoulFire.)
            if (pos.y <= 0) return false;
            const BlockID below = world->GetBlock(pos.x, pos.y - 1, pos.z);
            const Block& belowDef = BlockRegistry::Get(below);
            return belowDef.opaque;
        }

        // ── FlintAndSteel — mirrors FlintAndSteelItem.java:26-58 ────────────
        UseResult UseOn_FlintAndSteel(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    here = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            // MC: `if (!CampfireBlock.canLight(state) && !CandleBlock.canLight(state)
            //         && !CandleCakeBlock.canLight(state)) { ...spawn fire... }
            //     else { ...light campfire/candle... }`
            //
            // The relight branch runs FIRST and skips the canBePlacedAt check
            // entirely — clicking an unlit campfire lights the campfire, it
            // does not try to put a fire block next to it.
            //
            // CampfireBlock.canLight also requires !WATERLOGGED; this engine
            // has no waterlogging, so that term is always true here.
            //
            // Candles and candle cakes are still missing — they need their own
            // `lit` (and `candles`) properties, which nothing declares yet.
            if (here == BlockID::Campfire || here == BlockID::SoulCampfire) {
                const auto& def = BlockRegistry::GetStateDefinition(here);
                const BlockState cur = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
                if (cur.GetValueByName("lit") == "false") {
                    PlaySound("item.flintandsteel.use", pos);
                    BlockRegistry::BlockStateDefinition::PropertyMap props;
                    props["facing"] = std::string(cur.GetValueByName("facing"));
                    props["lit"]    = "true";
                    if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, here,
                                             World::UpdateFlags::All,
                                             def.IndexOf(props))) {
                        return UseResult::Fail;
                    }
                    GameEventEmit("block_change", pos);
                    HurtAndBreak(stack, 1, ctx.hand);
                    return UseResult::Success;
                }
            }

            // Not a relightable block — fall into the place-fire path:

            const glm::ivec3 firePos = ctx.getPlacementPos();
            if (!CanFireBePlacedAt(ctx.world, firePos)) {
                return UseResult::Fail;  // matches MC: explicit FAIL when the surface won't hold fire
            }

            // MC: `level.playSound(player, relativePos, FLINTANDSTEEL_USE,
            //      BLOCKS, 1.0F, level.getRandom().nextFloat() * 0.4F + 0.8F)`
            PlaySound("flint_and_steel.use", firePos);

            // MC: `BlockState fireState = BaseFireBlock.getState(level, relativePos);
            //      level.setBlock(relativePos, fireState, 11);`
            // We don't have soul fire / fire age state — plain BlockID::Fire.
            const bool ok = ctx.world->SetBlock(firePos.x, firePos.y, firePos.z,
                                                BlockID::Fire,
                                                World::UpdateFlags::All);
            if (!ok) return UseResult::Fail;

            // MC: `level.gameEvent(player, GameEvent.BLOCK_PLACE, pos)`
            //  — note MC uses the CLICKED pos, not the fire's pos.
            GameEventEmit("block_place", pos);

            // TODO(advancements): CriteriaTriggers.PLACED_BLOCK.trigger(serverPlayer, firePos, itemStack);

            // MC: `if (player instanceof ServerPlayer) itemStack.hurtAndBreak(1, player, hand.asEquipmentSlot());`
            HurtAndBreak(stack, 1, ctx.hand);

            return UseResult::Success;
        }

        // ── Hoe — mirrors HoeItem.java + TILLABLES static block ────────────
        // MC: `static { TILLABLES = Map.of(
        //         GRASS_BLOCK,  (onlyIfAirAbove,  changeIntoState(FARMLAND.defaultBlockState()))
        //         DIRT_PATH,    (onlyIfAirAbove,  changeIntoState(FARMLAND.defaultBlockState()))
        //         DIRT,         (onlyIfAirAbove,  changeIntoState(FARMLAND.defaultBlockState()))
        //         COARSE_DIRT,  (onlyIfAirAbove,  changeIntoState(DIRT.defaultBlockState()))
        //         ROOTED_DIRT,  ((ctx) -> true,   changeIntoStateAndDropItem(DIRT.defaultBlockState(), HANGING_ROOTS))
        //     ); }`
        // - onlyIfAirAbove: face != DOWN AND block-above is air (HoeItem.java:72)
        // - changeIntoState: setBlock + gameEvent BLOCK_CHANGE
        // - changeIntoStateAndDropItem: same + popResourceFromFace
        UseResult UseOn_Hoe(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    src = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            // Resolve TILLABLES entry. MC stores (predicate, action) pairs;
            // we collapse to (newBlock, dropItem-or-Air, requiresAirAbove).
            BlockID newBlock = BlockID::Air;
            BlockID dropItem = BlockID::Air;  // BlockID::Air sentinel = no drop
            bool    requiresAirAbove = true;
            switch (src) {
                case BlockID::Grass:       newBlock = BlockID::Farmland;                                         break;
                case BlockID::DirtPath:    newBlock = BlockID::Farmland;                                         break;
                case BlockID::Dirt:        newBlock = BlockID::Farmland;                                         break;
                case BlockID::CoarseDirt:  newBlock = BlockID::Dirt;                                             break;
                case BlockID::RootedDirt:  newBlock = BlockID::Dirt; dropItem = BlockID::HangingRoots;
                                           requiresAirAbove = false;                                              break;
                default: return UseResult::Pass;  // not tillable
            }

            // Predicate check (HoeItem.java:34-40): the per-entry Predicate.
            // For all entries except ROOTED_DIRT it's `onlyIfAirAbove`.
            if (requiresAirAbove) {
                // MC `onlyIfAirAbove` (HoeItem.java:72):
                //   face != DOWN && level.getBlockState(pos.above()).isAir()
                if (ctx.hitResult.face == 0) return UseResult::Pass;
                if (ctx.world->IsValidPosition(pos.x, pos.y + 1, pos.z)) {
                    const BlockID above = ctx.world->GetBlock(pos.x, pos.y + 1, pos.z);
                    if (above != BlockID::Air) return UseResult::Pass;
                }
            }

            // MC: play HOE_TILL sound on BOTH client and server side
            // (it's outside the !isClientSide guard).
            PlaySound("item.hoe.till", pos);

            // MC: `if (!level.isClientSide()) { action.accept(context); ...hurtAndBreak... }`
            // Always runs server-side for us.
            const bool ok = ctx.world->SetBlock(pos.x, pos.y, pos.z, newBlock,
                                                World::UpdateFlags::All);
            if (!ok) return UseResult::Fail;

            // MC `changeIntoState`: gameEvent BLOCK_CHANGE.
            GameEventEmit("block_change", pos);

            // MC `changeIntoStateAndDropItem`: also pop the drop item from the clicked face.
            if (dropItem != BlockID::Air) {
                PopResourceFromFace(ctx.world, pos, ctx.hitResult.face, dropItem);
            }

            HurtAndBreak(stack, 1, ctx.hand);
            return UseResult::Success;
        }

        // ── Shovel — mirrors ShovelItem.java + FLATTENABLES static block ───
        // MC: `static { FLATTENABLES = Map.of(
        //         GRASS_BLOCK,  DIRT_PATH.defaultBlockState(),
        //         DIRT,         DIRT_PATH.defaultBlockState(),
        //         PODZOL,       DIRT_PATH.defaultBlockState(),
        //         COARSE_DIRT,  DIRT_PATH.defaultBlockState(),
        //         MYCELIUM,     DIRT_PATH.defaultBlockState(),
        //         ROOTED_DIRT,  DIRT_PATH.defaultBlockState()
        //     ); }`
        UseResult UseOn_Shovel(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            // ShovelItem.java:33 — `if (face == DOWN) return PASS`
            if (ctx.hitResult.face == 0) return UseResult::Pass;

            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    src = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            // FLATTENABLES lookup → DIRT_PATH for all of these.
            const bool flattenable =
                   src == BlockID::Grass
                || src == BlockID::Dirt
                || src == BlockID::Podzol
                || src == BlockID::CoarseDirt
                || src == BlockID::Mycelium
                || src == BlockID::RootedDirt;

            BlockID newBlock = BlockID::Air;
            if (flattenable) {
                // MC: `if (newState != null && level.getBlockState(pos.above()).isAir())`
                //   — the block-above-must-be-air check is INSIDE the "is in
                //     FLATTENABLES" branch.
                if (ctx.world->IsValidPosition(pos.x, pos.y + 1, pos.z)) {
                    const BlockID above = ctx.world->GetBlock(pos.x, pos.y + 1, pos.z);
                    if (above != BlockID::Air) return UseResult::Pass;
                }
                PlaySound("item.shovel.flatten", pos);
                newBlock = BlockID::DirtPath;
            } else if (src == BlockID::Campfire || src == BlockID::SoulCampfire) {
                // MC: `else if (block instanceof CampfireBlock && state.getValue(LIT))`
                //   — extinguish a lit campfire by toggling LIT to false.
                //
                // A PROPERTY edit, not a block swap: the two-argument SetBlock
                // would write the default state, and a campfire's default is
                // LIT=true (CampfireBlock.java:77) — so it would relight the
                // fire the shovel just put out.
                const auto& def = BlockRegistry::GetStateDefinition(src);
                const BlockState cur = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
                if (cur.GetValueByName("lit") != "true") return UseResult::Pass;

                PlaySound("block.fire.extinguish", pos);
                BlockRegistry::BlockStateDefinition::PropertyMap props;
                props["facing"] = std::string(cur.GetValueByName("facing"));
                props["lit"]    = "false";
                if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, src,
                                         World::UpdateFlags::All, def.IndexOf(props))) {
                    return UseResult::Fail;
                }
                GameEventEmit("block_change", pos);
                HurtAndBreak(stack, 1, ctx.hand);
                return UseResult::Success;
                // MC also calls CampfireBlock.dowse, which is particles + a
                // game event only — the food stays on the fire and simply
                // stops cooking, which falls out of the state change above
                // because the block state is what picks the cooking ticker.
            } else {
                return UseResult::Pass;
            }

            // MC: `if (!level.isClientSide()) { setBlock; gameEvent; hurtAndBreak; }`
            const bool ok = ctx.world->SetBlock(pos.x, pos.y, pos.z, newBlock,
                                                World::UpdateFlags::All);
            if (!ok) return UseResult::Fail;
            GameEventEmit("block_change", pos);
            HurtAndBreak(stack, 1, ctx.hand);
            return UseResult::Success;
        }
        // ── Axe — mirrors AxeItem.java:38-105 ───────────────────────────────
        // Strip / de-oxidize / wax-off, resolved in that order
        // (evaluateNewBlockState, :70-90).

        // STRIPPABLES (AxeItem.java:107-108) — MC preserves the log's AXIS
        // property; we have no block states, so orientation resets to the
        // single stripped BlockID (documented deviation).
        BlockID StrippedVariant(BlockID src) {
            switch (src) {
                case BlockID::OakWood:        return BlockID::StrippedOakWood;
                case BlockID::OakLog:         return BlockID::StrippedOakLog;
                case BlockID::DarkOakWood:    return BlockID::StrippedDarkOakWood;
                case BlockID::DarkOakLog:     return BlockID::StrippedDarkOakLog;
                case BlockID::PaleOakWood:    return BlockID::StrippedPaleOakWood;
                case BlockID::PaleOakLog:     return BlockID::StrippedPaleOakLog;
                case BlockID::AcaciaWood:     return BlockID::StrippedAcaciaWood;
                case BlockID::AcaciaLog:      return BlockID::StrippedAcaciaLog;
                case BlockID::CherryWood:     return BlockID::StrippedCherryWood;
                case BlockID::CherryLog:      return BlockID::StrippedCherryLog;
                case BlockID::BirchWood:      return BlockID::StrippedBirchWood;
                case BlockID::BirchLog:       return BlockID::StrippedBirchLog;
                case BlockID::JungleWood:     return BlockID::StrippedJungleWood;
                case BlockID::JungleLog:      return BlockID::StrippedJungleLog;
                case BlockID::SpruceWood:     return BlockID::StrippedSpruceWood;
                case BlockID::SpruceLog:      return BlockID::StrippedSpruceLog;
                case BlockID::WarpedStem:     return BlockID::StrippedWarpedStem;
                case BlockID::WarpedHyphae:   return BlockID::StrippedWarpedHyphae;
                case BlockID::CrimsonStem:    return BlockID::StrippedCrimsonStem;
                case BlockID::CrimsonHyphae:  return BlockID::StrippedCrimsonHyphae;
                case BlockID::MangroveWood:   return BlockID::StrippedMangroveWood;
                case BlockID::MangroveLog:    return BlockID::StrippedMangroveLog;
                case BlockID::BambooBlock:    return BlockID::StrippedBambooBlock;
                default:                      return BlockID::Air;
            }
        }

        // Copper weathering families — one row per shape, four oxidation
        // stages + the matching waxed stages. Backs WeatheringCopper's
        // NEXT/PREVIOUS_BY_BLOCK and HoneycombItem.WAXABLES/WAX_OFF_BY_BLOCK.
        // (Slab-top promoted variants and stairs/slabs of the same families
        // follow identically via their own rows.)
        struct CopperFamily {
            BlockID stage[4];   // base, exposed, weathered, oxidized
            BlockID waxed[4];   // waxed counterparts, same order
        };
        const CopperFamily* CopperFamilies(size_t& count) {
            static const CopperFamily families[] = {
                {{BlockID::CopperBlock,     BlockID::ExposedCopper,           BlockID::WeatheredCopper,           BlockID::OxidizedCopper},
                 {BlockID::WaxedCopperBlock,BlockID::WaxedExposedCopper,      BlockID::WaxedWeatheredCopper,      BlockID::WaxedOxidizedCopper}},
                {{BlockID::ChiseledCopper,  BlockID::ExposedChiseledCopper,   BlockID::WeatheredChiseledCopper,   BlockID::OxidizedChiseledCopper},
                 {BlockID::WaxedChiseledCopper, BlockID::WaxedExposedChiseledCopper, BlockID::WaxedWeatheredChiseledCopper, BlockID::WaxedOxidizedChiseledCopper}},
                {{BlockID::CutCopper,       BlockID::ExposedCutCopper,        BlockID::WeatheredCutCopper,        BlockID::OxidizedCutCopper},
                 {BlockID::WaxedCutCopper,  BlockID::WaxedExposedCutCopper,   BlockID::WaxedWeatheredCutCopper,   BlockID::WaxedOxidizedCutCopper}},
                {{BlockID::CutCopperSlab,   BlockID::ExposedCutCopperSlab,    BlockID::WeatheredCutCopperSlab,    BlockID::OxidizedCutCopperSlab},
                 {BlockID::WaxedCutCopperSlab, BlockID::WaxedExposedCutCopperSlab, BlockID::WaxedWeatheredCutCopperSlab, BlockID::WaxedOxidizedCutCopperSlab}},
                {{BlockID::CutCopperStairs, BlockID::ExposedCutCopperStairs,  BlockID::WeatheredCutCopperStairs,  BlockID::OxidizedCutCopperStairs},
                 {BlockID::WaxedCutCopperStairs, BlockID::WaxedExposedCutCopperStairs, BlockID::WaxedWeatheredCutCopperStairs, BlockID::WaxedOxidizedCutCopperStairs}},
                {{BlockID::CopperGrate,     BlockID::ExposedCopperGrate,      BlockID::WeatheredCopperGrate,      BlockID::OxidizedCopperGrate},
                 {BlockID::WaxedCopperGrate,BlockID::WaxedExposedCopperGrate, BlockID::WaxedWeatheredCopperGrate, BlockID::WaxedOxidizedCopperGrate}},
                {{BlockID::CopperBulb,      BlockID::ExposedCopperBulb,       BlockID::WeatheredCopperBulb,       BlockID::OxidizedCopperBulb},
                 {BlockID::WaxedCopperBulb, BlockID::WaxedExposedCopperBulb,  BlockID::WaxedWeatheredCopperBulb,  BlockID::WaxedOxidizedCopperBulb}},
                {{BlockID::CopperDoor,      BlockID::ExposedCopperDoor,       BlockID::WeatheredCopperDoor,       BlockID::OxidizedCopperDoor},
                 {BlockID::WaxedCopperDoor, BlockID::WaxedExposedCopperDoor,  BlockID::WaxedWeatheredCopperDoor,  BlockID::WaxedOxidizedCopperDoor}},
                {{BlockID::CopperTrapdoor,  BlockID::ExposedCopperTrapdoor,   BlockID::WeatheredCopperTrapdoor,   BlockID::OxidizedCopperTrapdoor},
                 {BlockID::WaxedCopperTrapdoor, BlockID::WaxedExposedCopperTrapdoor, BlockID::WaxedWeatheredCopperTrapdoor, BlockID::WaxedOxidizedCopperTrapdoor}},
                {{BlockID::CopperBars,      BlockID::ExposedCopperBars,       BlockID::WeatheredCopperBars,       BlockID::OxidizedCopperBars},
                 {BlockID::WaxedCopperBars, BlockID::WaxedExposedCopperBars,  BlockID::WaxedWeatheredCopperBars,  BlockID::WaxedOxidizedCopperBars}},
                {{BlockID::CopperChain,     BlockID::ExposedCopperChain,      BlockID::WeatheredCopperChain,      BlockID::OxidizedCopperChain},
                 {BlockID::WaxedCopperChain,BlockID::WaxedExposedCopperChain, BlockID::WaxedWeatheredCopperChain, BlockID::WaxedOxidizedCopperChain}},
                {{BlockID::CopperLantern,   BlockID::ExposedCopperLantern,    BlockID::WeatheredCopperLantern,    BlockID::OxidizedCopperLantern},
                 {BlockID::WaxedCopperLantern, BlockID::WaxedExposedCopperLantern, BlockID::WaxedWeatheredCopperLantern, BlockID::WaxedOxidizedCopperLantern}},
                {{BlockID::LightningRod,    BlockID::ExposedLightningRod,     BlockID::WeatheredLightningRod,     BlockID::OxidizedLightningRod},
                 {BlockID::WaxedLightningRod, BlockID::WaxedExposedLightningRod, BlockID::WaxedWeatheredLightningRod, BlockID::WaxedOxidizedLightningRod}},
            };
            count = sizeof(families) / sizeof(families[0]);
            return families;
        }

        // WeatheringCopper.getPrevious — one oxidation stage back.
        BlockID CopperScrapedVariant(BlockID src) {
            size_t count = 0;
            const CopperFamily* families = CopperFamilies(count);
            for (size_t f = 0; f < count; ++f) {
                for (int s = 1; s < 4; ++s) {
                    if (families[f].stage[s] == src) return families[f].stage[s - 1];
                }
            }
            return BlockID::Air;
        }

        // HoneycombItem.WAXABLES (HoneycombItem.java:29) — unwaxed → waxed.
        BlockID WaxedVariant(BlockID src) {
            size_t count = 0;
            const CopperFamily* families = CopperFamilies(count);
            for (size_t f = 0; f < count; ++f) {
                for (int s = 0; s < 4; ++s) {
                    if (families[f].stage[s] == src) return families[f].waxed[s];
                }
            }
            return BlockID::Air;
        }

        // HoneycombItem.WAX_OFF_BY_BLOCK (:30) — waxed → unwaxed.
        BlockID WaxOffVariant(BlockID src) {
            size_t count = 0;
            const CopperFamily* families = CopperFamilies(count);
            for (size_t f = 0; f < count; ++f) {
                for (int s = 0; s < 4; ++s) {
                    if (families[f].waxed[s] == src) return families[f].stage[s];
                }
            }
            return BlockID::Air;
        }

        UseResult UseOn_Axe(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;

            // playerHasBlockingItemUseIntent (AxeItem.java:65-68): main-hand
            // use + offhand holds a BLOCKS_ATTACKS item + not sneaking →
            // PASS so the click raises the shield instead of stripping.
            if (ctx.hand == 0 && ctx.player && !ctx.player->IsSneaking()) {
                const Game::ItemStack& offhand = ctx.player->getItemInHand(1);
                if (!offhand.IsEmpty()
                    && offhand.get(DataComponents::BLOCKS_ATTACKS)) {
                    return UseResult::Pass;
                }
            }

            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    src = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            // evaluateNewBlockState (:70-90) — strip, then scrape, then wax-off.
            BlockID newBlock = StrippedVariant(src);
            if (newBlock != BlockID::Air) {
                PlaySound("item.axe.strip", pos);                       // :73
            } else {
                newBlock = CopperScrapedVariant(src);
                if (newBlock != BlockID::Air) {
                    PlaySound("item.axe.scrape", pos);                  // :78
                    // levelEvent 3005 (scrape particles) — particle system TODO.
                } else {
                    newBlock = WaxOffVariant(src);
                    if (newBlock != BlockID::Air) {
                        PlaySound("item.axe.wax_off", pos);             // :83
                        // levelEvent 3004 (wax-off particles) — TODO.
                    } else {
                        return UseResult::Pass;                         // :86
                    }
                }
            }

            // :54 setBlock(flags 11) / :55 gameEvent / :57 hurtAndBreak.
            if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, newBlock,
                                     World::UpdateFlags::All)) {
                return UseResult::Fail;
            }
            GameEventEmit("block_change", pos);
            HurtAndBreak(stack, 1, ctx.hand);
            return UseResult::Success;                                  // :60
        }

        // ── Dye on a sheep — mirrors DyeItem.interactLivingEntity ───────────
        //
        // MC gates on three things, and all three matter: the sheep must be
        // ALIVE, NOT SHEARED (there is no wool to dye), and not already that
        // colour (so the click falls through instead of eating a dye for
        // nothing).
        UseResult InteractEntity_Dye(ItemStack& stack, LivingEntity& target, uint8_t color) {
            auto* sheep = dynamic_cast<Sheep*>(&target);
            if (!sheep) return UseResult::Pass;
            if (!sheep->IsAlive() || sheep->IsSheared()) return UseResult::Pass;
            if (sheep->GetColor() == color) return UseResult::Pass;

            PlaySound("item.dye.use", sheep->BlockPosition());
            sheep->SetColor(color);

            // MC itemStack.shrink(1). Creative is restored by the dispatch's
            // count snapshot, the same way the useOn path handles it.
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            return UseResult::Success;
        }

        // One thunk per colour — the callback table stores plain function
        // pointers, so the colour has to come from the function's identity
        // rather than from captured state.
        template <uint8_t Color>
        UseResult InteractEntity_DyeColor(ItemStack& stack, LivingEntity& target) {
            return InteractEntity_Dye(stack, target, Color);
        }

        // ── Spawn eggs — mirrors SpawnEggItem.java:52-89 ────────────────────
        UseResult UseOn_SpawnEgg(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Fail;

            // :54 `if (!(level instanceof ServerLevel)) return SUCCESS;`
            // The client cannot predict an entity into existence — it only
            // exists once the server sends it — and in single-player both
            // sides share a process, so running on through here would spawn
            // twice. Returning Success still consumes the click, which is what
            // stops the placement fallback from firing behind it.
            if (ctx.world->IsClientSide()) return UseResult::Success;

            const EntityTypeId type = SpawnEggEntityType(stack.itemId);
            if (type == EntityTypeId::Count) return UseResult::Fail;   // :62 FAIL

            // MC's first branch is a Spawner block entity (a monster spawner
            // reprogrammed by the egg). There is no Spawner in this port, so
            // the else branch is the only one.
            const glm::ivec3 clicked = ctx.hitResult.blockPos;

            // :80-85 spawn INSIDE the clicked cell when it has no collision
            // (tall grass, a flower, air), otherwise on the face that was hit.
            const BlockID clickedId = ctx.world->GetBlock(clicked.x, clicked.y, clicked.z);
            const bool    solid     = BlockRegistry::HasCollision(clickedId);

            const glm::ivec3 spawnPos = solid ? ctx.getPlacementPos() : clicked;
            // :87 `!Objects.equals(pos, spawnPos) && clickedFace == Direction.UP`
            const bool movedUp = solid && ctx.hitResult.face == 1;   // 1 = up

            // :91-105 spawnMob. The peaceful-difficulty rule and the placement
            // slide live server-side with the mob managers; see
            // IntegratedServer::SpawnMobFromItemUse.
            if (SpawnMobFromItem(type, spawnPos, /*tryMoveDown=*/true, movedUp)) {
                // :101 itemStack.consume(1, user) — only on a successful spawn,
                // so an egg rejected by difficulty is not eaten. Creative is
                // restored by the dispatch's stack snapshot.
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
                GameEventEmit("entity_place", spawnPos);
            }

            // :104 SUCCESS regardless — MC swallows the click either way.
            return UseResult::Success;
        }

        // ── Honeycomb — mirrors HoneycombItem.java:46-73 ────────────────────
        UseResult UseOn_Honeycomb(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    src = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            const BlockID waxed = WaxedVariant(src);   // WAXABLES lookup (:50)
            if (waxed == BlockID::Air) return UseResult::Pass;   // :72 PASS

            // :57 itemInHand.shrink(1) — creative restored by the dispatch's
            // snapshot (ServerPlayerGameMode-style). Note that Clear() wipes
            // the id, not just the count, which is why the dispatch snapshots
            // the WHOLE stack; see PlayerSession::HandleUseItemOn.
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            // :58 setBlock(11); :60 levelEvent 3003 (wax-on particles) TODO.
            if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, waxed,
                                     World::UpdateFlags::All)) {
                return UseResult::Fail;
            }
            PlaySound("item.honeycomb.wax_on", pos);
            GameEventEmit("block_change", pos);
            return UseResult::Success;
        }

        // ── Bone meal — mirrors BoneMealItem.java:35-61 ─────────────────────
        // Two of MC's three branches are here: growCrop (any BonemealableBlock
        // — crops, stems, bamboo, cocoa, berry bushes) and the grass-block
        // scatter. The water/seagrass branch (:83-136) is still skipped; it
        // needs fluid simulation and coral biome tags.
        UseResult UseOn_BoneMeal(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    src = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            // BoneMealItem.growCrop (BoneMealItem.java:63-81):
            //   if (block instanceof BonemealableBlock b
            //       && b.isValidBonemealTarget(level, pos, state)) {
            //       if (b.isBonemealSuccess(...)) b.performBonemeal(...);
            //       stack.shrink(1);
            //       return true;
            //   }
            // isBonemealSuccess is `true` for every block modelled here, so it
            // folds into the target test — see BlockRegistry.hpp's note on the
            // bone-meal hook pair.
            {
                const Block& def = BlockRegistry::Get(src);
                if (def.performBonemeal && def.isValidBonemealTarget) {
                    const BlockState state = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
                    if (def.isValidBonemealTarget(*ctx.world, pos, state)) {
                        // Seeded per use rather than kept as a static: this
                        // runs on the client for prediction too, and a shared
                        // stream between the two would drift apart anyway.
                        // Growth amount is re-derived from the server's own
                        // roll when its block update lands.
                        JavaRandom random(static_cast<int64_t>(pos.x) * 341873128712LL +
                                          static_cast<int64_t>(pos.z) * 132897987541LL +
                                          static_cast<int64_t>(pos.y));
                        def.performBonemeal(*ctx.world, pos, state, random);

                        // MC: `level.levelEvent(1505, pos, 15)` →
                        // BoneMealItem.addGrowthParticles spawns 15
                        // happy_villager particles inside the block.
                        // TODO(particles): no particle system yet.
                        PlaySound("item.bone_meal.use", pos);
                        // MC: itemStack.shrink(1). Creative is handled by the
                        // dispatch's whole-stack snapshot, NOT here — and it
                        // has to be the whole stack, because Clear() below
                        // wipes the id as well as the count.
                        stack.count -= 1;
                        if (stack.count <= 0) stack.Clear();
                        return UseResult::Success;
                    }
                    // A valid bonemealable block that is already fully grown:
                    // MC falls through to the water branch and ultimately
                    // returns PASS, consuming nothing.
                    return UseResult::Pass;
                }
            }

            if (src != BlockID::Grass) {
                return UseResult::Pass;
            }

            // GrassBlock.performBonemeal — 128 random-walk attempts from the
            // block above; each valid air cell over grass gets short grass,
            // or 1-in-8 a flower (the biome flower feature collapsed to the
            // plains dandelion/poppy pair).
            static std::mt19937 rng{std::random_device{}()};
            auto nextInt = [&](int bound) {
                return std::uniform_int_distribution<int>(0, bound - 1)(rng);
            };

            bool anyPlaced = false;
            const glm::ivec3 above = pos + glm::ivec3(0, 1, 0);
            for (int i = 0; i < 128; ++i) {
                glm::ivec3 current = above;
                bool walkValid = true;
                for (int j = 0; j < i / 16; ++j) {
                    current += glm::ivec3(nextInt(3) - 1,
                                          (nextInt(3) - 1) * nextInt(3) / 2,
                                          nextInt(3) - 1);
                    if (!ctx.world->IsValidPosition(current.x, current.y - 1, current.z)
                        || ctx.world->GetBlock(current.x, current.y - 1, current.z)
                               != BlockID::Grass) {
                        walkValid = false;
                        break;
                    }
                }
                if (!walkValid) continue;
                if (!ctx.world->IsValidPosition(current.x, current.y, current.z)) continue;
                if (ctx.world->GetBlock(current.x, current.y, current.z) != BlockID::Air) continue;

                BlockID plant = BlockID::ShortGrass;
                if (nextInt(8) == 0) {
                    plant = (nextInt(2) == 0) ? BlockID::Dandelion : BlockID::Poppy;
                }
                if (ctx.world->SetBlock(current.x, current.y, current.z, plant,
                                        World::UpdateFlags::All)) {
                    anyPlaced = true;
                }
            }

            if (!anyPlaced) return UseResult::Fail;

            // :43 levelEvent 1505 (bone-meal particles) — particle system TODO.
            PlaySound("item.bone_meal.use", pos);
            // :73 itemStack.shrink(1) — creative restored by the dispatch's
            // whole-stack snapshot (Clear() wipes the id too, so a count-only
            // restore would lose the last bone meal).
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            return UseResult::Success;
        }

        // ── Buckets — mirror BucketItem.java:43-95 ──────────────────────────
        // MC buckets are `use` (air-click) items that do their OWN POV
        // raycast (Item.getPlayerPOVHitResult, Item.java:354-358) with a
        // fluid mode: SOURCE_ONLY for the empty bucket (ray stops on fluid),
        // NONE for filled buckets (fluids invisible, ray hits the ground).
        // The client-side world raycast treats fluids as non-solid, so
        // clicking water arrives here as an air-use — exactly the flow MC
        // has. Implemented as a small server-side DDA over World::GetBlock.
        struct BucketHit {
            glm::ivec3 pos;
            glm::ivec3 beforePos;   // the cell the ray was in before the hit
            BlockID    block;
        };
        // Does the ray meet any box of this block's shape inside `cell`?
        //
        // MC Item.getPlayerPOVHitResult clips with ClipContext.Block.OUTLINE,
        // i.e. against `BlockBehaviour.getShape` — the same VoxelShape the
        // selection box is drawn from. So "did I hit this block" is a question
        // about its GEOMETRY, not about whether its cell is non-air.
        bool RayMeetsShape(const glm::vec3& origin, const glm::vec3& dir,
                           const glm::ivec3& cell, BlockState state,
                           float maxDistance) {
            const auto shapes = BlockRegistry::GetBlockShapeSet(state);
            for (const auto& box : shapes) {
                const glm::vec3 mn = glm::vec3(cell) + box.min;
                const glm::vec3 mx = glm::vec3(cell) + box.max;
                float tNear = 0.0f, tFar = maxDistance;
                bool  hit   = true;
                for (int axis = 0; axis < 3 && hit; ++axis) {
                    if (std::abs(dir[axis]) < 1e-6f) {
                        // Parallel to this pair of planes — must already be
                        // between them.
                        if (origin[axis] < mn[axis] || origin[axis] > mx[axis]) hit = false;
                        continue;
                    }
                    float t1 = (mn[axis] - origin[axis]) / dir[axis];
                    float t2 = (mx[axis] - origin[axis]) / dir[axis];
                    if (t1 > t2) std::swap(t1, t2);
                    tNear = std::max(tNear, t1);
                    tFar  = std::min(tFar,  t2);
                    if (tNear > tFar) hit = false;
                }
                if (hit) return true;
            }
            return false;
        }

        std::optional<BucketHit> BucketClip(ILevelWrite* world,
                                            const IUsePlayer& player,
                                            bool stopOnFluid) {
            // Eye + direction from the server-authoritative rotation (the
            // UseItemC2S handler snapped it to the click's exact aim).
            const glm::dvec3 p = player.getPosition();
            const glm::vec3 eye(static_cast<float>(p.x),
                                static_cast<float>(p.y) + 1.62f,
                                static_cast<float>(p.z));
            const glm::vec3 dir =
                Mth::ViewVector(player.getPitch(), player.getYaw());

            constexpr float kReach = 5.0f;   // blockInteractionRange

            // Walk cells with a DDA and test each one's SHAPE, rather than the
            // fixed-step "first non-air cell" march this used to be. That march
            // had two failure modes, and partial blocks hit both: a coarse step
            // can miss a cell the ray only clips, and a cell was accepted with
            // no geometry test at all — so the empty half of a stair, the gap
            // under a top slab and the air around a torch all counted as hits.
            // The bucket then emptied into a block the crosshair was never on,
            // and WHERE on the block you had clicked decided whether the two
            // agreed.
            glm::ivec3 cell(static_cast<int>(std::floor(eye.x)),
                            static_cast<int>(std::floor(eye.y)),
                            static_cast<int>(std::floor(eye.z)));
            glm::ivec3 stepDir(dir.x > 0.0f ? 1 : -1,
                               dir.y > 0.0f ? 1 : -1,
                               dir.z > 0.0f ? 1 : -1);
            glm::vec3 tMax(0.0f), tDelta(0.0f);
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(dir[axis]) < 1e-6f) {
                    tMax[axis]   = std::numeric_limits<float>::infinity();
                    tDelta[axis] = std::numeric_limits<float>::infinity();
                } else {
                    const float bound = (dir[axis] > 0.0f)
                                            ? std::floor(eye[axis]) + 1.0f
                                            : std::floor(eye[axis]);
                    tMax[axis]   = (bound - eye[axis]) / dir[axis];
                    tDelta[axis] = 1.0f / std::abs(dir[axis]);
                }
            }

            glm::ivec3 prevCell = cell;
            float travelled = 0.0f;
            while (travelled <= kReach) {
                if (!world->IsValidPosition(cell.x, cell.y, cell.z)) return std::nullopt;
                const BlockID block = world->GetBlock(cell.x, cell.y, cell.z);
                const bool isFluid = (block == BlockID::Water || block == BlockID::Lava);
                if (block != BlockID::Air && (!isFluid || stopOnFluid)) {
                    // Fluids fill their cell, so they need no shape test; every
                    // other block is clipped against its real geometry.
                    if (isFluid ||
                        RayMeetsShape(eye, dir, cell,
                                      world->GetBlockState(cell.x, cell.y, cell.z), kReach)) {
                        return BucketHit{cell, prevCell, block};
                    }
                }
                prevCell = cell;
                if (tMax.x < tMax.y && tMax.x < tMax.z) {
                    cell.x += stepDir.x; travelled = tMax.x; tMax.x += tDelta.x;
                } else if (tMax.y < tMax.z) {
                    cell.y += stepDir.y; travelled = tMax.y; tMax.y += tDelta.y;
                } else {
                    cell.z += stepDir.z; travelled = tMax.z; tMax.z += tDelta.z;
                }
            }
            return std::nullopt;
        }

        // Empty bucket — BucketItem.java:43-74 (fill path).
        UseResult Use_EmptyBucket(ILevelWrite* world, IUsePlayer* player,
                                  uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/true);   // :45 SOURCE_ONLY
            if (!hit) return UseResult::Pass;                              // :46-47
            // MC BucketItem.java:49-53 dispatches on `state.getBlock()
            // instanceof BucketPickup`, which every SimpleWaterloggedBlock is.
            // So a waterlogged fence, ladder or stair hands over its water and
            // STAYS PUT — the block is not removed, only its flag is cleared
            // (SimpleWaterloggedBlock.pickupBlock:36-44). The always-water
            // blocks are deliberately not included: kelp and seagrass are not
            // BucketPickup in vanilla either, so a bucket does nothing to them.
            const BlockState hitState =
                world->GetBlockState(hit->pos.x, hit->pos.y, hit->pos.z);
            if (BlockRegistry::IsWaterloggable(hit->block) &&
                BlockRegistry::ContainsWater(hitState)) {
                const BlockState dryState = BlockRegistry::WithWaterlogged(hitState, false);
                world->SetBlock(hit->pos.x, hit->pos.y, hit->pos.z, dryState,
                                World::UpdateFlags::All);
                // SimpleWaterloggedBlock.pickupBlock:39-41 — a block that
                // cannot survive without its water is destroyed on the spot.
                // Vanilla's case is coral, which dies out of water. A no-op
                // until coral survival is modelled, wired now so it is not a
                // second thing to remember when it is.
                if (!CanSurviveAt(*world, hit->pos, dryState)) {
                    world->SetBlock(hit->pos.x, hit->pos.y, hit->pos.z,
                                    BlockID::Air, World::UpdateFlags::All);
                }
                PlaySound("item.bucket.fill", hit->pos);
                if (!player->isCreative()) {
                    stack = ItemStack(Items::WaterBucket, 1);
                    player->markSlotDirty(player->handSlotIndex(hand));
                }
                return UseResult::Success;
            }

            if (hit->block != BlockID::Water && hit->block != BlockID::Lava) {
                return UseResult::Pass;                                    // :93-94 (BLOCK hit → pass)
            }

            // :55-74 — pickupBlock (no fluid levels: every water/lava cell is
            // a source), sound, transform bucket → filled variant.
            world->SetBlock(hit->pos.x, hit->pos.y, hit->pos.z, BlockID::Air,
                            World::UpdateFlags::All);
            PlaySound(hit->block == BlockID::Lava ? "item.bucket.fill_lava"
                                                  : "item.bucket.fill",
                      hit->pos);
            // ItemUtils.createFilledResult (:62): creative keeps the empty
            // bucket, survival transforms it. Component patch reset — a
            // fresh filled bucket carries no per-stack state.
            if (!player->isCreative()) {
                stack = ItemStack(hit->block == BlockID::Lava ? Items::LavaBucket
                                                              : Items::WaterBucket, 1);
                player->markSlotDirty(player->handSlotIndex(hand));
            }
            return UseResult::Success;
        }

        // Filled bucket — BucketItem.java:76-95 + emptyContents (:104-179,
        // simplified: no fluid simulation, sources are static cubes).
        UseResult Use_FilledBucket(ILevelWrite* world, IUsePlayer* player,
                                   uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            const bool isLava = (stack.itemId == Items::LavaBucket);
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/false);  // ClipContext.Fluid.NONE
            if (!hit) return UseResult::Pass;

            // MC BucketItem.java:83-84:
            //   BlockPos target = state.getBlock() instanceof LiquidBlockContainer
            //                     && this.content == Fluids.WATER ? pos : relativePos;
            //
            // Pouring WATER onto a waterloggable block fills the block itself
            // rather than the cell in front of it — that is how you waterlog a
            // fence or a stair. Lava is excluded by the `content == WATER`
            // clause, exactly as vanilla has it, so a lava bucket still pours
            // into the adjacent cell.
            if (!isLava && BlockRegistry::IsWaterloggable(hit->block)) {
                const BlockState hitState =
                    world->GetBlockState(hit->pos.x, hit->pos.y, hit->pos.z);
                // SimpleWaterloggedBlock.placeLiquid:24 refuses when the block
                // is already waterlogged, and the refusal propagates all the
                // way out as a failed use — the bucket is not consumed.
                if (BlockRegistry::ContainsWater(hitState)) {
                    return UseResult::Fail;
                }
                world->SetBlock(hit->pos.x, hit->pos.y, hit->pos.z,
                                BlockRegistry::WithWaterlogged(hitState, true),
                                World::UpdateFlags::All);
                PlaySound("item.bucket.empty", hit->pos);
                if (!player->isCreative()) {
                    stack = ItemStack(Items::Bucket, 1);
                    player->markSlotDirty(player->handSlotIndex(hand));
                }
                return UseResult::Success;
            }

            // :76-77 — target = clicked cell when replaceable, else the cell
            // the ray came from (pos.relative(direction)).
            const Game::Block& hitDef = BlockRegistry::Get(hit->block);
            (void)hitDef;
            glm::ivec3 target = hit->beforePos;
            const BlockID targetBlock =
                world->IsValidPosition(target.x, target.y, target.z)
                    ? world->GetBlock(target.x, target.y, target.z)
                    : BlockID::Bedrock;
            // emptyContents' mayInteract/replaceable gate (:161-163): air and
            // fluids are pour-into-able; anything else is blocked.
            if (targetBlock != BlockID::Air
                && targetBlock != BlockID::Water && targetBlock != BlockID::Lava) {
                return UseResult::Fail;                                     // :88
            }

            if (!world->SetBlock(target.x, target.y, target.z,
                                 isLava ? BlockID::Lava : BlockID::Water,
                                 World::UpdateFlags::All)) {
                return UseResult::Fail;
            }
            PlaySound(isLava ? "item.bucket.empty_lava" : "item.bucket.empty",
                      target);                                              // :175-179
            // :97-99 getEmptySuccessItem — creative keeps the filled bucket.
            if (!player->isCreative()) {
                stack = ItemStack(Items::Bucket, 1);
                player->markSlotDirty(player->handSlotIndex(hand));
            }
            return UseResult::Success;                                      // :90
        }

    } // namespace

    void ItemRegistry_RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems) {
        auto wireUseOn = [&](ItemID id, ItemUseOnFn fn) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) it->second.useOn = fn;
        };

        // ── Seeds: pure items that place a block ────────────────────────────
        //
        // In MC every one of these is an ordinary BlockItem whose *name* simply
        // differs from its block's (Items.java:1548,
        // createBlockItemWithCustomItemName). This engine derives a block item's
        // ItemID from its BlockID, so a mismatched pair cannot be expressed that
        // way and needs the explicit `placesBlock` link instead.
        //
        // Wiring it here rather than giving each seed a `useOn` is deliberate:
        // placement then flows through the SAME BlockItem fallback the server
        // already runs (PlayerSession::HandleUseItemOn) and the SAME client
        // prediction (ClientPlayerController::ComputePredictedPlacement) as any
        // other block — including the survive/replace/obstruction gates. A
        // per-seed `useOn` would have bypassed prediction entirely, since
        // ComputePredictedPlacement refuses to predict items that carry one.
        //
        // Whether a seed may be planted where you clicked is NOT decided here —
        // that is CanSurviveOn in BlockPlacement.cpp (MC mayPlaceOn).
        auto wirePlacesBlock = [&](ItemID id, BlockID block) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) it->second.placesBlock = block;
        };
        wirePlacesBlock(Items::WheatSeeds,       BlockID::Wheat);
        wirePlacesBlock(Items::BeetrootSeeds,    BlockID::Beetroots);
        wirePlacesBlock(Items::Carrot,           BlockID::Carrots);
        wirePlacesBlock(Items::Potato,           BlockID::Potatoes);
        wirePlacesBlock(Items::MelonSeeds,       BlockID::MelonStem);
        wirePlacesBlock(Items::PumpkinSeeds,     BlockID::PumpkinStem);
        wirePlacesBlock(Items::NetherWart,       BlockID::NetherWart);
        wirePlacesBlock(Items::TorchflowerSeeds, BlockID::TorchflowerCrop);
        wirePlacesBlock(Items::PitcherPod,       BlockID::PitcherCrop);
        wirePlacesBlock(Items::CocoaBeans,       BlockID::Cocoa);
        wirePlacesBlock(Items::SweetBerries,     BlockID::SweetBerryBush);
        // Not seeds, but the identical shape of mismatch — MC builds all of
        // these with createBlockItemWithCustomItemName too (Items.java).
        wirePlacesBlock(Items::Redstone,         BlockID::RedstoneWire);
        wirePlacesBlock(Items::String,           BlockID::Tripwire);
        wirePlacesBlock(Items::ResinClump,       BlockID::ResinClump);
        // The one member of that family left out: glow_berries -> cave_vines.
        // Cave vines hang from a CEILING and carry `age`/`berries` states this
        // engine does not model, so the mapping alone would place a block that
        // cannot survive or render correctly. Wire it up with the block.
        // Sugar cane, cactus and bamboo are already block items — their ItemID
        // and BlockID coincide, so AsBlockID() finds them without a row here.

        // FlintAndSteel — single variant.
        wireUseOn(Items::FlintAndSteel, &UseOn_FlintAndSteel);

        // Every hoe tier shares the till behaviour. Tool material (mining
        // speed, durability, attack damage) is a per-item property MC reads
        // from the Item.Properties.hoe(material, …) builder; we don't model
        // tool materials yet, so all tiers behave identically until we do.
        for (ItemID id : {
                Items::WoodenHoe, Items::CopperHoe, Items::StoneHoe,
                Items::GoldenHoe, Items::IronHoe,   Items::DiamondHoe,
                Items::NetheriteHoe }) {
            wireUseOn(id, &UseOn_Hoe);
        }

        // Same for every shovel tier.
        for (ItemID id : {
                Items::WoodenShovel, Items::CopperShovel, Items::StoneShovel,
                Items::GoldenShovel, Items::IronShovel,   Items::DiamondShovel,
                Items::NetheriteShovel }) {
            wireUseOn(id, &UseOn_Shovel);
        }

        // Every axe tier shares strip/scrape/wax-off (AxeItem.java:38-105).
        for (ItemID id : {
                Items::WoodenAxe, Items::CopperAxe, Items::StoneAxe,
                Items::GoldenAxe, Items::IronAxe,   Items::DiamondAxe,
                Items::NetheriteAxe }) {
            wireUseOn(id, &UseOn_Axe);
        }

        // Honeycomb waxing (HoneycombItem.java:46-73).
        wireUseOn(Items::Honeycomb, &UseOn_Honeycomb);

        // Bone meal — growCrop + grass-scatter branches (BoneMealItem.java:35-81).
        wireUseOn(Items::BoneMeal, &UseOn_BoneMeal);

        // Dyes, in DyeColor ordinal order — the index IS the wool value the
        // sheep stores, so the table must not be reordered.
        auto wireInteract = [&](ItemID id, ItemInteractEntityFn fn) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) it->second.interactLivingEntity = fn;
        };
        wireInteract(Items::WhiteDye,     &InteractEntity_DyeColor<0>);
        wireInteract(Items::OrangeDye,    &InteractEntity_DyeColor<1>);
        wireInteract(Items::MagentaDye,   &InteractEntity_DyeColor<2>);
        wireInteract(Items::LightBlueDye, &InteractEntity_DyeColor<3>);
        wireInteract(Items::YellowDye,    &InteractEntity_DyeColor<4>);
        wireInteract(Items::LimeDye,      &InteractEntity_DyeColor<5>);
        wireInteract(Items::PinkDye,      &InteractEntity_DyeColor<6>);
        wireInteract(Items::GrayDye,      &InteractEntity_DyeColor<7>);
        wireInteract(Items::LightGrayDye, &InteractEntity_DyeColor<8>);
        wireInteract(Items::CyanDye,      &InteractEntity_DyeColor<9>);
        wireInteract(Items::PurpleDye,    &InteractEntity_DyeColor<10>);
        wireInteract(Items::BlueDye,      &InteractEntity_DyeColor<11>);
        wireInteract(Items::BrownDye,     &InteractEntity_DyeColor<12>);
        wireInteract(Items::GreenDye,     &InteractEntity_DyeColor<13>);
        wireInteract(Items::RedDye,       &InteractEntity_DyeColor<14>);
        wireInteract(Items::BlackDye,     &InteractEntity_DyeColor<15>);

        // Spawn eggs, one row per implemented mob (SpawnEggItem.java:52).
        // Eggs for mobs this port does not have are deliberately left
        // unwired — see SpawnEggs.hpp.
        for (const SpawnEggEntry& egg : kSpawnEggTable) {
            wireUseOn(egg.item, &UseOn_SpawnEgg);
        }

        // Buckets — `use` (air-click + own POV raycast), not `useOn`,
        // matching BucketItem's design (BucketItem.java:43-95).
        auto wireUse = [&](ItemID id, ItemUseFn fn) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) it->second.use = fn;
        };
        wireUse(Items::Bucket,      &Use_EmptyBucket);
        wireUse(Items::WaterBucket, &Use_FilledBucket);
        wireUse(Items::LavaBucket,  &Use_FilledBucket);
        // Filled buckets stack to 1 (Items.java `.stacksTo(1)` on all buckets;
        // the empty bucket stacks to 16).
        if (auto it = pureItems.find(Items::Bucket); it != pureItems.end())
            it->second.maxStackSize = 16;
        for (ItemID id : { Items::WaterBucket, Items::LavaBucket }) {
            if (auto it = pureItems.find(id); it != pureItems.end())
                it->second.maxStackSize = 1;
        }

        // ── Tool data component (MC parity) ─────────────────────────────────
        // Wire the TOOL data component onto every pickaxe / axe / shovel / hoe
        // / sword / shears. Mirrors MC's `Items.java` builder usage of
        // `Item.Properties.tool(material, blocks, speed)`. The Tool component
        // is what Item.getDestroySpeed (Item.java:191) reads to multiply the
        // player's base mining speed.
        //
        // Speeds: wood 2.0, gold 12.0, stone 4.0, iron 6.0, diamond 8.0,
        // netherite 9.0 (vanilla Tiers.java). Copper isn't a vanilla tier — we
        // size it between stone and iron (speed 5.0, level=stone) so the items
        // remain useful.
        auto setTool = [&](ItemID id, Tool t) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) {
                it->second.defaultComponents.set(DataComponents::TOOL, t);
            }
        };

        // Pickaxes
        setTool(Items::WoodenPickaxe,    Tool{ToolType::Pickaxe, MiningTier::Wood,      2.0f});
        setTool(Items::CopperPickaxe,    Tool{ToolType::Pickaxe, MiningTier::Stone,     5.0f});
        setTool(Items::StonePickaxe,     Tool{ToolType::Pickaxe, MiningTier::Stone,     4.0f});
        setTool(Items::GoldenPickaxe,    Tool{ToolType::Pickaxe, MiningTier::Gold,     12.0f});
        setTool(Items::IronPickaxe,      Tool{ToolType::Pickaxe, MiningTier::Iron,      6.0f});
        setTool(Items::DiamondPickaxe,   Tool{ToolType::Pickaxe, MiningTier::Diamond,   8.0f});
        setTool(Items::NetheritePickaxe, Tool{ToolType::Pickaxe, MiningTier::Netherite, 9.0f});
        // Axes
        setTool(Items::WoodenAxe,        Tool{ToolType::Axe,     MiningTier::Wood,      2.0f});
        setTool(Items::CopperAxe,        Tool{ToolType::Axe,     MiningTier::Stone,     5.0f});
        setTool(Items::StoneAxe,         Tool{ToolType::Axe,     MiningTier::Stone,     4.0f});
        setTool(Items::GoldenAxe,        Tool{ToolType::Axe,     MiningTier::Gold,     12.0f});
        setTool(Items::IronAxe,          Tool{ToolType::Axe,     MiningTier::Iron,      6.0f});
        setTool(Items::DiamondAxe,       Tool{ToolType::Axe,     MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteAxe,     Tool{ToolType::Axe,     MiningTier::Netherite, 9.0f});
        // Shovels
        setTool(Items::WoodenShovel,     Tool{ToolType::Shovel,  MiningTier::Wood,      2.0f});
        setTool(Items::CopperShovel,     Tool{ToolType::Shovel,  MiningTier::Stone,     5.0f});
        setTool(Items::StoneShovel,      Tool{ToolType::Shovel,  MiningTier::Stone,     4.0f});
        setTool(Items::GoldenShovel,     Tool{ToolType::Shovel,  MiningTier::Gold,     12.0f});
        setTool(Items::IronShovel,       Tool{ToolType::Shovel,  MiningTier::Iron,      6.0f});
        setTool(Items::DiamondShovel,    Tool{ToolType::Shovel,  MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteShovel,  Tool{ToolType::Shovel,  MiningTier::Netherite, 9.0f});
        // Hoes
        setTool(Items::WoodenHoe,        Tool{ToolType::Hoe,     MiningTier::Wood,      2.0f});
        setTool(Items::CopperHoe,        Tool{ToolType::Hoe,     MiningTier::Stone,     5.0f});
        setTool(Items::StoneHoe,         Tool{ToolType::Hoe,     MiningTier::Stone,     4.0f});
        setTool(Items::GoldenHoe,        Tool{ToolType::Hoe,     MiningTier::Gold,     12.0f});
        setTool(Items::IronHoe,          Tool{ToolType::Hoe,     MiningTier::Iron,      6.0f});
        setTool(Items::DiamondHoe,       Tool{ToolType::Hoe,     MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteHoe,     Tool{ToolType::Hoe,     MiningTier::Netherite, 9.0f});
        // Swords (used for cobweb / bamboo speedup in MC)
        setTool(Items::WoodenSword,      Tool{ToolType::Sword,   MiningTier::Wood,      2.0f});
        setTool(Items::CopperSword,      Tool{ToolType::Sword,   MiningTier::Stone,     5.0f});
        setTool(Items::StoneSword,       Tool{ToolType::Sword,   MiningTier::Stone,     4.0f});
        setTool(Items::GoldenSword,      Tool{ToolType::Sword,   MiningTier::Gold,     12.0f});
        setTool(Items::IronSword,        Tool{ToolType::Sword,   MiningTier::Iron,      6.0f});
        setTool(Items::DiamondSword,     Tool{ToolType::Sword,   MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteSword,   Tool{ToolType::Sword,   MiningTier::Netherite, 9.0f});
        // Shears (single tier; MC speed = 1.5 against most, 15.0 vs wool/leaves)
        // No useOn wired: ShearsItem's interactions (beehive honeycombs,
        // pumpkin carving) all produce item DROPS — BLOCKED on item entities.
        setTool(Items::Shears,           Tool{ToolType::Shears,  MiningTier::Iron,      1.5f});

        // ── RARITY defaults (name-line tooltip color) ───────────────────────
        // Rows verbatim from Items.java `.rarity(...)` builders in THIS
        // vendored snapshot (note: golden_apple has NO rarity here — plain
        // COMMON; enchanted_golden_apple and enchanted_book are RARE, not the
        // older EPIC/UNCOMMON). Extend freely as more items matter.
        {
            auto setRarity = [&](ItemID id, Rarity r) {
                auto it = pureItems.find(id);
                if (it != pureItems.end()) {
                    it->second.defaultComponents.set(DataComponents::RARITY, r);
                }
            };
            setRarity(Items::ChainmailHelmet,      Rarity::UNCOMMON); // Items.java:2572
            setRarity(Items::ChainmailChestplate,  Rarity::UNCOMMON); // :2573
            setRarity(Items::ChainmailLeggings,    Rarity::UNCOMMON); // :2574
            setRarity(Items::ChainmailBoots,       Rarity::UNCOMMON); // :2575
            setRarity(Items::EnchantedGoldenApple, Rarity::RARE);     // :2597
            setRarity(Items::RecoveryCompass,      Rarity::UNCOMMON); // :2645
            setRarity(Items::Elytra,               Rarity::EPIC);     // :2472
            setRarity(Items::ExperienceBottle,     Rarity::UNCOMMON); // :2827
            setRarity(Items::Mace,                 Rarity::EPIC);     // :2833
            setRarity(Items::NetherStar,           Rarity::RARE);     // :2850
            setRarity(Items::EnchantedBook,        Rarity::RARE);     // :2854
            setRarity(Items::DragonBreath,         Rarity::UNCOMMON); // :2900
            setRarity(Items::TotemOfUndying,       Rarity::UNCOMMON); // :2913
            setRarity(Items::Trident,              Rarity::RARE);     // :2941
            setRarity(Items::CreeperBannerPattern, Rarity::UNCOMMON); // :2953
            setRarity(Items::MojangBannerPattern,  Rarity::RARE);     // :2955
        }

        // Food/consumable component defaults (FoodDefs.cpp).
        ItemRegistry_RegisterFoods(pureItems);

        // Armor/shield equipment defaults (EquipmentBehavior.cpp).
        ItemRegistry_RegisterEquipment(pureItems);

        // Bundle click behaviours + crafting remainders (BundleBehavior.cpp).
        ItemRegistry_RegisterBundles(pureItems);

        Log::Info("[ItemRegistry] Wired use-behaviour callbacks "
                  "(FlintAndSteel, 7 hoes, 7 shovels) + Tool components on 36 tool items");
    }

} // namespace Game
