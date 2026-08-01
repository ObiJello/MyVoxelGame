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
#include "../data/DataComponents.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../world/level/World.hpp"
#include "../core/Log.hpp"
#include "server/player/ServerPlayer.hpp"  // bucket POV raycast needs eye pos/rotation
                                           // (common→server precedent: PortalGunBehavior.cpp)

#include <cmath>
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
        // hanging_roots item). We don't have item entities yet, so log it.
        void PopResourceFromFace(World* world, const glm::ivec3& pos,
                                 int face, BlockID dropId) {
            (void)world; (void)face;
            // TODO(item-entities): spawn an ItemEntity(dropId * 1) at face center
            //                      with small outward velocity, and broadcast.
            Log::Debug("[ItemDrop] block %u at (%d,%d,%d) (face %d) — TODO: spawn item entity",
                       static_cast<unsigned>(dropId), pos.x, pos.y, pos.z, face);
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
        bool CanFireBePlacedAt(World* world, const glm::ivec3& pos) {
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
            // TODO(block-state-properties): we don't have block state properties
            // (campfire LIT, candle LIT) yet. Once those land, this branch should
            // run FIRST: if the clicked block is an UN-lit campfire, candle, or
            // candle-cake, set its LIT property to true here, play sound,
            // gameEvent, hurtAndBreak, and return SUCCESS — without going
            // through the canBePlacedAt check below.
            //
            // For now we always fall into the place-fire path:

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
            } else if (src == BlockID::Campfire) {
                // MC: `else if (block instanceof CampfireBlock && state.getValue(LIT))`
                //   — extinguish a lit campfire by toggling LIT to false, plus
                //     a level event 1009 (extinguish particles+sound) and the
                //     CampfireBlock.dowse helper that drops contents.
                //
                // TODO(block-state-properties): once block state properties
                // exist, gate this on `LIT == true`, then setValue(LIT, false)
                // instead of replacing the whole block. Until then, leave the
                // campfire unmodified (return PASS) — flattening it to dirt
                // would be wrong, and we can't read LIT to know if it's lit.
                return UseResult::Pass;
                // Future:
                //   if (state.getValue(CAMPFIRE_LIT)) {
                //       LevelEvent(1009, pos);  // extinguish particles+sizzle
                //       CampfireDowse(world, pos);  // drops items in the campfire
                //       newBlock = state.with(LIT, false);  // requires real BlockState
                //   } else return UseResult::Pass;
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

        // ── Honeycomb — mirrors HoneycombItem.java:46-73 ────────────────────
        UseResult UseOn_Honeycomb(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    src = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            const BlockID waxed = WaxedVariant(src);   // WAXABLES lookup (:50)
            if (waxed == BlockID::Air) return UseResult::Pass;   // :72 PASS

            // :57 itemInHand.shrink(1) — creative count restored by the
            // dispatch's snapshot (ServerPlayerGameMode-style).
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

        // ── Bone meal — mirrors BoneMealItem.java:35-61, grass branch only ──
        // Crops/saplings need growth-stage block states (single BlockIDs
        // here) — BLOCKED: no block growth stages. The grass-block branch
        // ports GrassBlock.performBonemeal's 128-attempt random-walk scatter.
        UseResult UseOn_BoneMeal(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID    src = ctx.world->GetBlock(pos.x, pos.y, pos.z);

            if (src != BlockID::Grass) {
                // growCrop (BoneMealItem.java:63-81) needs BonemealableBlock —
                // BLOCKED: no block growth stages (wheat/carrots/… are single
                // BlockIDs). The water/seagrass branch (:83-136) is skipped
                // too — no fluid simulation.
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
            // :73 itemStack.shrink(1) — creative restored by dispatch snapshot.
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
        std::optional<BucketHit> BucketClip(World* world,
                                            const Server::ServerPlayer& player,
                                            bool stopOnFluid) {
            // Eye + direction from the server-authoritative rotation (the
            // UseItemC2S handler snapped it to the click's exact aim).
            const glm::dvec3 p = player.getPosition();
            const glm::vec3 eye(static_cast<float>(p.x),
                                static_cast<float>(p.y) + 1.62f,
                                static_cast<float>(p.z));
            const float yaw   = glm::radians(player.getYaw());
            const float pitch = glm::radians(player.getPitch());
            const glm::vec3 dir(std::cos(yaw) * std::cos(pitch),
                                std::sin(pitch),
                                std::sin(yaw) * std::cos(pitch));

            constexpr float kReach = 5.0f;   // blockInteractionRange
            constexpr float kStep  = 0.05f;  // fine march — fluid cells are full cubes
            glm::ivec3 prevCell(
                static_cast<int>(std::floor(eye.x)),
                static_cast<int>(std::floor(eye.y)),
                static_cast<int>(std::floor(eye.z)));
            for (float t = 0.0f; t <= kReach; t += kStep) {
                const glm::vec3 sample = eye + dir * t;
                const glm::ivec3 cell(
                    static_cast<int>(std::floor(sample.x)),
                    static_cast<int>(std::floor(sample.y)),
                    static_cast<int>(std::floor(sample.z)));
                if (cell == prevCell && t > 0.0f) continue;
                if (!world->IsValidPosition(cell.x, cell.y, cell.z)) return std::nullopt;
                const BlockID block = world->GetBlock(cell.x, cell.y, cell.z);
                const bool isFluid = (block == BlockID::Water || block == BlockID::Lava);
                const bool isSolid = (block != BlockID::Air) && !isFluid;
                if (isSolid || (isFluid && stopOnFluid)) {
                    return BucketHit{cell, prevCell, block};
                }
                prevCell = cell;
            }
            return std::nullopt;
        }

        // Empty bucket — BucketItem.java:43-74 (fill path).
        UseResult Use_EmptyBucket(World* world, Server::ServerPlayer* player,
                                  uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/true);   // :45 SOURCE_ONLY
            if (!hit) return UseResult::Pass;                              // :46-47
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
            if (player->getGameMode() != Server::GameMode::CREATIVE) {
                stack = ItemStack(hit->block == BlockID::Lava ? Items::LavaBucket
                                                              : Items::WaterBucket, 1);
                player->markSlotDirty(player->handSlotIndex(hand));
            }
            return UseResult::Success;
        }

        // Filled bucket — BucketItem.java:76-95 + emptyContents (:104-179,
        // simplified: no fluid simulation, sources are static cubes).
        UseResult Use_FilledBucket(World* world, Server::ServerPlayer* player,
                                   uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            const bool isLava = (stack.itemId == Items::LavaBucket);
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/false);  // ClipContext.Fluid.NONE
            if (!hit) return UseResult::Pass;

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
            if (player->getGameMode() != Server::GameMode::CREATIVE) {
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

        // Bone meal — grass-scatter branch only (BoneMealItem.java:35-61;
        // crop growth BLOCKED on block growth stages).
        wireUseOn(Items::BoneMeal, &UseOn_BoneMeal);

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
