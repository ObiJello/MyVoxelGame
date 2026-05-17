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
#include "../world/block/BlockRegistry.hpp"
#include "../world/level/World.hpp"
#include "../core/Log.hpp"

namespace Game {

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

        Log::Info("[ItemRegistry] Wired use-behaviour callbacks "
                  "(FlintAndSteel, 7 hoes, 7 shovels)");
    }

} // namespace Game
