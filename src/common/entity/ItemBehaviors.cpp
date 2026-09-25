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
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "GeneratedItemList.hpp"
#include "SpawnEggs.hpp"
#include "common/entity/decoration/HangingEntity.hpp"
#include "EndCrystal.hpp"
#include "FallingBlockEntity.hpp"
#include "PrimedTnt.hpp"
#include "projectile/Projectile.hpp"
#include "mobs/SulfurCube.hpp"
#include "../world/level/WorldMobSpawn.hpp"
#include "mobs/Animals.hpp"
#include "ArmorStand.hpp"
#include "../data/DataComponents.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../world/block/BlockPlacement.hpp"
#include "../world/fluid/FlowingFluid.hpp"
#include "../world/level/DimensionId.hpp"
#include "../world/level/World.hpp"
#include "../world/level/WorldDrops.hpp"
#include "../world/level/HushItems.hpp"
#include "../world/level/AurelithQuest.hpp"
#include "../world/portal/ModPortalBehaviors.hpp"
#include "../world/portal/PortalFamily.hpp"
#include "../world/portal/PortalShape.hpp"
#include "../world/portal/PortalState.hpp"
#include "../world/portal/EndPortalFrame.hpp"
#include "../core/JavaRandom.hpp"
#include "../core/Mth.hpp"
#include "../core/Log.hpp"
#include "IUsePlayer.hpp"
#include "../world/block/entity/SpawnerBlockEntity.hpp"
#include "../world/level/GameRules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <string>
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

    // Implemented in alchemy/PotionItems.cpp — POTION_CONTENTS defaults on the
    // four potion items, the drinkable potion's CONSUMABLE, the throws, and
    // the stew's SUSPICIOUS_STEW_EFFECTS.
    void ItemRegistry_RegisterPotionItems(std::unordered_map<ItemID, Item>& pureItems);
    void ItemRegistry_RegisterBookItems(std::unordered_map<ItemID, Item>& pureItems);   // BookItems.cpp

    namespace {

        // ── Common helpers ──────────────────────────────────────────────────

        // Sounds go through the level (ILevelWrite::PlaySound, MC
        // Level.playSound) with MC's `except` argument: an item used on a
        // block passes the player, because this chain runs twice — on the
        // client as the prediction (which plays it for the user at once) and
        // on the server (which sends it to everyone else).

        // MC level.getRandom().nextFloat(), or a midpoint where the level has
        // no random (never on either real side).
        float LevelRandomFloat(ILevelWrite* level) {
            JavaRandom* r = level ? level->Random() : nullptr;
            return r ? r->NextFloat() : 0.5f;
        }

        // Mirrors MC `Level.gameEvent(GameEvent, BlockPos, Context)`
        // (Level.java:1129). Sculk sensors / wardens listen on these. We don't
        // simulate them yet; this is a no-op marker that captures the intent.
        void GameEventEmit(const char* eventName, const glm::ivec3& pos) {
            (void)eventName; (void)pos;
            // TODO(game-events): once GameEvent system exists, broadcast here.
        }

        // Mirrors MC `itemStack.hurtAndBreak(amount, player, hand)` from an
        // Item.useOn: server only (the client's prediction never wears the
        // item), none in creative, and a break shrinks the stack and plays the
        // break effects through the player (Game::HurtAndBreak, Item.hpp).
        void UseOnHurtAndBreak(ItemStack& stack, int amount, const UseOnContext& ctx) {
            HurtAndBreak(stack, amount, ctx.world, ctx.player, ctx.hand);
        }

        // MC `Block.popResourceFromFace(level, pos, face, itemStack)` —
        // spawns an item entity at the clicked face with a small velocity
        // outward. Used by HoeItem when tilling rooted_dirt (drops a
        // hanging_roots item).
        void PopResourceFromFace(ILevelWrite* world, const glm::ivec3& pos,
                                 int face, BlockID dropId) {
            DropItemStackFromFace(world ? world->GetDimension()
                                        : DimensionId::Overworld,
                                  pos, face, ItemStack(dropId, 1));
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

        // ── BaseFireBlock.isPortal (BaseFireBlock.java:181) ─────────────────
        // The second half of canBePlacedAt: fire may also go somewhere it
        // could never survive on its own, as long as that somewhere is the
        // inside of an empty obsidian portal frame. This is what lets you
        // light a portal by clicking the obsidian at head height, three blocks
        // off the ground — the classic way everyone actually does it.
        //
        // `forwardDirection` is the face of the block that was clicked. MC
        // derives the preferred portal axis from it (the axis perpendicular to
        // the clicked face, in the horizontal plane) and falls back to a
        // random horizontal axis for a vertical face. The random pick costs
        // nothing here — findEmptyPortalShape tries the other axis anyway when
        // the first one fails — so a fixed X keeps the behaviour deterministic
        // between the client's prediction and the server's authority, which
        // matters more.
        bool IsInEmptyPortalFrame(ILevelWrite* world, const glm::ivec3& pos,
                                  int clickedFace) {
            if (!world) return false;
            if (!DimensionAllowsNetherPortal(world->GetDimension())) return false;

            // MC scans all six neighbours for obsidian first — a cheap reject
            // that keeps the frame walk off every fire placement in the world.
            static constexpr Direction kFaces[6] = {
                Direction::Down, Direction::Up, Direction::North,
                Direction::South, Direction::West, Direction::East,
            };
            bool hasObsidian = false;
            for (Direction d : kFaces) {
                const glm::ivec3 n{ pos.x + StepX(d), pos.y + StepY(d), pos.z + StepZ(d) };
                if (world->GetBlock(n.x, n.y, n.z) == BlockID::Obsidian) {
                    hasObsidian = true;
                    break;
                }
            }
            if (!hasObsidian) return false;

            const Direction face = static_cast<Direction>(
                clickedFace >= 0 && clickedFace <= 5 ? clickedFace
                                                     : static_cast<int>(Direction::North));
            const Axis preferredAxis = IsHorizontal(face)
                ? AxisOf(ClockWise(face))    // MC getCounterClockWise().getAxis();
                                             // either turn gives the same AXIS
                : Axis::X;
            return PortalShape::FindEmptyPortalShape(*world, pos, preferredAxis).has_value();
        }

        bool CanFireBePlacedAt(ILevelWrite* world, const glm::ivec3& pos,
                               int clickedFace) {
            if (!world) return false;
            if (!world->IsValidPosition(pos.x, pos.y, pos.z)) return false;
            if (world->GetBlock(pos.x, pos.y, pos.z) != BlockID::Air) return false;
            // Block below must be solid for fire to survive on its top face.
            // (Skipping the soul-fire variant + isValidFireLocation flammable
            // neighbour check — both produce the same Boolean for solid floors,
            // which is the overwhelming common case. Refine when we add fire
            // spread behaviour and SoulFire.)
            if (pos.y > 0) {
                const BlockID below = world->GetBlock(pos.x, pos.y - 1, pos.z);
                const Block& belowDef = BlockRegistry::Get(below);
                if (belowDef.opaque) return true;
            }
            // MC: `getState(level, pos).canSurvive(level, pos) || isPortal(...)`
            return IsInEmptyPortalFrame(world, pos, clickedFace);
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
                    // FlintAndSteelItem.useOn:54 — playSound(player, pos,
                    // FLINTANDSTEEL_USE, BLOCKS, 1.0, nextFloat * 0.4 + 0.8).
                    ctx.world->PlaySound(ctx.player, pos, SoundEvents::FLINTANDSTEEL_USE, SoundSource::Blocks,
                                         1.0f, LevelRandomFloat(ctx.world) * 0.4f + 0.8f);
                    BlockRegistry::BlockStateDefinition::PropertyMap props;
                    props["facing"] = std::string(cur.GetValueByName("facing"));
                    props["lit"]    = "true";
                    if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, here,
                                             World::UpdateFlags::All,
                                             def.IndexOf(props))) {
                        return UseResult::Fail;
                    }
                    GameEventEmit("block_change", pos);
                    UseOnHurtAndBreak(stack, 1, ctx);
                    return UseResult::Success;
                }
            }

            // Not a relightable block — fall into the place-fire path:

            const glm::ivec3 firePos = ctx.getPlacementPos();
            // MC passes the clicked FACE here (FlintAndSteelItem.java:41 →
            // canBePlacedAt(level, relativePos, context.getClickedFace())); it
            // is what picks the preferred portal axis when the target is the
            // inside of an obsidian frame.
            if (!CanFireBePlacedAt(ctx.world, firePos, ctx.hitResult.face)) {
                return UseResult::Fail;  // matches MC: explicit FAIL when the surface won't hold fire
            }

            // MC: `level.playSound(player, relativePos, FLINTANDSTEEL_USE,
            //      BLOCKS, 1.0F, level.getRandom().nextFloat() * 0.4F + 0.8F)`
            ctx.world->PlaySound(ctx.player, firePos, SoundEvents::FLINTANDSTEEL_USE, SoundSource::Blocks,
                                 1.0f, LevelRandomFloat(ctx.world) * 0.4f + 0.8f);

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
            UseOnHurtAndBreak(stack, 1, ctx);

            return UseResult::Success;
        }

        // ── EnderEye.useOn — mirrors EnderEyeItem.java:36-70 ────────────────
        //
        // Puts an eye into an empty end_portal_frame and, if that completed
        // the ring, fills the 3x3 interior with end_portal blocks.
        UseResult UseOn_EnderEye(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockState target = ctx.world->GetBlockState(pos.x, pos.y, pos.z);

            // MC returns PASS, not FAIL, for anything else — and it matters:
            // FAIL stops the dispatch, which would break the throw-the-eye
            // path whenever the player happened to be aiming at a block.
            if (!target.Is(BlockID::EndPortalFrame)) return UseResult::Pass;
            if (EndPortalFrame::HasEye(target))       return UseResult::Pass;

            // MC: `if (level.isClientSide()) return SUCCESS;`. The client
            // predicts the arm swing and nothing else — filling nine cells
            // with end_portal on a prediction that the server then rejects
            // would leave a portal the server does not have.
            if (ctx.world->IsClientSide()) return UseResult::Success;

            const BlockState withEye = EndPortalFrame::WithEye(target, true);
            // MC uses flag 2 (clients only, no neighbour updates). Nothing
            // reacts to a frame block's shape, so the engine's MarkDirty —
            // which also reaches the change accumulator — is the same thing.
            if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, withEye,
                                     World::UpdateFlags::MarkDirty)) {
                return UseResult::Fail;
            }

            // MC: level.levelEvent(1503, pos, 0) — the eye-seated sound.
            PlayLevelEventSound(*ctx.world, nullptr, LevelEvent::END_PORTAL_FRAME_FILL, pos, 0,
                                ctx.world->Random());
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();

            // MC skips Block.pushEntitiesUp (the eye grows the block's
            // collision shape by 3 pixels, so anything standing on the frame
            // is nudged clear) and updateNeighbourForOutputSignal (comparators
            // read HAS_EYE). Neither system exists here.

            const auto match = EndPortalFrame::PortalShapePattern().Find(*ctx.world, pos);
            if (!match) return UseResult::Success;

            // EnderEyeItem.java:52 — the interior's minimum corner, in RAW
            // world offsets. That works because the pattern search is
            // deterministic: the only orientation an inward-facing ring can
            // match is forwards=DOWN, up=SOUTH, which puts frontTopLeft at the
            // ring's maximum X and Z. See BlockPattern.hpp.
            const glm::ivec3 base = match->frontTopLeft + glm::ivec3(-3, 0, -3);
            for (int x = 0; x < 3; ++x) {
                for (int z = 0; z < 3; ++z) {
                    const glm::ivec3 cell{ base.x + x, base.y, base.z + z };
                    // MC destroyBlock(pos, true) first: the pattern's interior
                    // predicate is ANY, so there can be something in the way,
                    // and vanilla drops it rather than deleting it.
                    ctx.world->SetBlock(cell.x, cell.y, cell.z, BlockID::EndPortal,
                                        World::UpdateFlags::All);
                }
            }

            // MC: globalLevelEvent(1038, blockPos.offset(1, 0, 1), 0) — the
            // portal-spawn fanfare, heard world-wide.
            PlayGlobalLevelEventSound(*ctx.world, LevelEvent::SOUND_END_PORTAL_SPAWN,
                                      base + glm::ivec3(1, 0, 1));
            return UseResult::Success;
        }

        // ── EchoShard.useOn — the Hush portal's ignition ─────────────────
        //
        // The engine's own item behaviour (vanilla's echo shard has no
        // useOn): an echo shard held against the ancient city's
        // reinforced-deepslate frame resonates and lights it, the way fire
        // lights obsidian. Shaped like UseOn_EnderEye above — the click
        // must land on the frame block itself, the client only swings, and
        // the server decides — with the fire block's two ignition routes
        // (immersive handler, vanilla PortalShape) behind it.
        //
        // The clicked block is the FRAME, not the interior, so the interior
        // has to be found from it: first the cell past the clicked face
        // (clicking the inside of a frame from within the opening), then
        // the four neighbours of the frame block perpendicular to the face
        // normal (clicking the frame's outer face — the interior lies
        // beside the block, not in front of it). The first seed that closes
        // a loop wins.
        UseResult UseOn_EchoShard(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockState target = ctx.world->GetBlockState(pos.x, pos.y, pos.z);

            // PASS, not FAIL, for anything but the frame block — the ender
            // eye's reasoning: FAIL stops the dispatch, and the shard has no
            // other use to fall through to today, but the block under the
            // click still gets its useWithoutItem.
            const PortalFamily& family = HushFamily();
            if (!family.isFrame(target.Block())) return UseResult::Pass;
            // A city frame in the Nether, or a hand-built one in the End,
            // does not resonate — the Hush's counterpart of
            // BaseFireBlock.inPortalDimension.
            if (!DimensionAllowsHushPortal(ctx.world->GetDimension())) return UseResult::Fail;

            // The client predicts the arm swing and nothing else, as with the
            // eye: lighting a 20x6 frame on a prediction the server then
            // rejects would leave a portal the server does not have.
            if (ctx.world->IsClientSide()) return UseResult::Success;

            const Direction face = static_cast<Direction>(
                ctx.hitResult.face >= 0 && ctx.hitResult.face <= 5 ? ctx.hitResult.face
                                                                    : static_cast<int>(Direction::North));
            const Axis faceAxis = AxisOf(face);
            // The vanilla walk's preferred axis, derived from the clicked face
            // the way FlintAndSteel's CanFireBePlacedAt does; FindEmptyPortal-
            // Shape tries the other one anyway.
            const Axis preferredAxis = IsHorizontal(face) ? AxisOf(ClockWise(face)) : Axis::X;

            glm::ivec3 seeds[5];
            int seedCount = 0;
            seeds[seedCount++] = ctx.getPlacementPos();
            for (int axis = 0; axis < 3; ++axis) {
                if (axis == static_cast<int>(faceAxis)) continue;
                glm::ivec3 step(0);
                step[axis] = 1;
                seeds[seedCount++] = pos + step;
                seeds[seedCount++] = pos - step;
            }

            bool lit = false;
            if (Portals::FamilyIsImmersive(family.id)) {
                // Never for the Hush today (Portals::FamilyIsImmersive: the
                // family is always vanilla, whatever /gamerule
                // immersive_portals says); kept so the family can be made
                // immersive in one place: the server's frame-lit handler
                // finds the loop and starts the far-side generation.
                if (auto handler = Portals::GetImmersiveFrameLitHandler()) {
                    for (int i = 0; i < seedCount && !lit; ++i) {
                        lit = handler(*ctx.world, seeds[i], family.id);
                    }
                }
            } else {
                // The Hush's portal: MC PortalShape's rectangle walk against
                // the Hush family (reinforced deepslate, up to 21×21),
                // filled with hush_portal blocks.
                for (int i = 0; i < seedCount && !lit; ++i) {
                    auto shape = PortalShape::FindEmptyPortalShape(*ctx.world, seeds[i],
                                                                   preferredAxis, family);
                    if (!shape) continue;
                    shape->CreatePortalBlocks(*ctx.world);
                    lit = true;
                }
            }
            if (!lit) return UseResult::Fail;

            // The shriek is the deep dark's own sound; a shard resonating
            // against the frame is the same voice (the engine event resolves
            // to it through assets/sound_overlays/obeycraft/world.json).
            // Server-only past the gate above, so everyone hears it from here.
            ctx.world->PlaySound(nullptr, pos, "obeycraft:block.hush_portal.ignite", SoundSource::Blocks,
                                 1.0f, 1.0f);
            // One shard per lighting; creative keeps its stack, as the
            // bucket and spawn-egg paths above do (MC `hasInfiniteMaterials`).
            if (!ctx.player || !ctx.player->isCreative()) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
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

            // MC HoeItem.useOn: playSound(player, pos, HOE_TILL, BLOCKS, 1, 1)
            // on BOTH sides (outside the !isClientSide guard) — the user's
            // prediction plays it, the server skips them.
            ctx.world->PlaySound(ctx.player, pos, SoundEvents::HOE_TILL, SoundSource::Blocks, 1.0f, 1.0f);

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

            UseOnHurtAndBreak(stack, 1, ctx);
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
                // ShovelItem.useOn: playSound(player, pos, SHOVEL_FLATTEN, BLOCKS).
                ctx.world->PlaySound(ctx.player, pos, SoundEvents::SHOVEL_FLATTEN, SoundSource::Blocks,
                                     1.0f, 1.0f);
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

                // ShovelItem.useOn: `if (!level.isClientSide()) level.levelEvent(
                // null, 1009, pos, 0)` — FIRE_EXTINGUISH, for everyone.
                if (!ctx.world->IsClientSide()) {
                    PlayLevelEventSound(*ctx.world, nullptr, LevelEvent::SOUND_EXTINGUISH_FIRE, pos, 0,
                                        ctx.world->Random());
                }
                BlockRegistry::BlockStateDefinition::PropertyMap props;
                props["facing"] = std::string(cur.GetValueByName("facing"));
                props["lit"]    = "false";
                if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, src,
                                         World::UpdateFlags::All, def.IndexOf(props))) {
                    return UseResult::Fail;
                }
                GameEventEmit("block_change", pos);
                UseOnHurtAndBreak(stack, 1, ctx);
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
            UseOnHurtAndBreak(stack, 1, ctx);
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
                // The Hush (docs/the-hush.md) — whisperwood has no "wood"
                // (bark-on-all-sides) variant, only the log.
                case BlockID::WhisperwoodLog: return BlockID::StrippedWhisperwoodLog;
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
                ctx.world->PlaySound(ctx.player, pos, SoundEvents::AXE_STRIP, SoundSource::Blocks,
                                     1.0f, 1.0f);                               // :73
            } else {
                newBlock = CopperScrapedVariant(src);
                if (newBlock != BlockID::Air) {
                    ctx.world->PlaySound(ctx.player, pos, SoundEvents::AXE_SCRAPE, SoundSource::Blocks,
                                         1.0f, 1.0f);                           // :78
                    // levelEvent 3005 (scrape particles) — particle system TODO.
                } else {
                    newBlock = WaxOffVariant(src);
                    if (newBlock != BlockID::Air) {
                        ctx.world->PlaySound(ctx.player, pos, SoundEvents::AXE_WAX_OFF, SoundSource::Blocks,
                                             1.0f, 1.0f);                       // :83
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
            UseOnHurtAndBreak(stack, 1, ctx);
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

            // MC DyeItem.interactLivingEntity:28 — playSound(player, sheep,
            // DYE_USE, PLAYERS): entity-bound. The interaction is the
            // server's alone here, so the user hears it from the server too.
            if (EntityLevel* lvl = sheep->Level()) {
                lvl->PlaySoundFromEntity(nullptr, *sheep, SoundEvents::DYE_USE, SoundSource::Players, 1.0f, 1.0f);
            }
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

        // ── Name tag — mirrors NameTagItem.interactLivingEntity ─────────────
        //
        // Only a RENAMED tag (an anvil-set CUSTOM_NAME) names anything; a
        // blank one passes, so the click falls through to the entity. The
        // target must be a type that saves (a named lightning bolt would
        // vanish with its name) and alive. The tracker sees the new name on
        // its next sweep and resends it with the mob's entity data.
        UseResult InteractEntity_NameTag(ItemStack& stack, LivingEntity& target) {
            std::optional<std::string> customName = stack.components.get(DataComponents::CUSTOM_NAME);
            if (!customName || !target.CanSerialize()) return UseResult::Pass;
            // MC's target is a LivingEntity; the hanging entities ride the
            // living pipeline here (HangingEntity.hpp) but are plain Entities
            // in MC, so a name tag passes them by.
            if (dynamic_cast<const HangingEntity*>(&target)) return UseResult::Pass;
            // The same for the other plain Entities this port runs as mobs:
            // primed TNT, a falling block, an End crystal and every
            // projectile never reach interactLivingEntity in MC.
            if (dynamic_cast<const PrimedTnt*>(&target) ||
                dynamic_cast<const FallingBlockEntity*>(&target) ||
                dynamic_cast<const EndCrystal*>(&target) ||
                dynamic_cast<const Projectile*>(&target)) {
                return UseResult::Pass;
            }
            if (!target.IsAlive()) return UseResult::Success;

            target.SetCustomName(std::move(customName));
            // `if (target instanceof Mob) mob.setPersistenceRequired()`. The
            // armor stand derives from this port's Mob but is a plain
            // LivingEntity in MC, so it takes the name and nothing else.
            if (auto* mob = dynamic_cast<Mob*>(&target); mob && !dynamic_cast<ArmorStand*>(mob)) {
                mob->SetPersistenceRequired(true);
            }

            // MC itemStack.shrink(1). Creative is restored by the dispatch's
            // count snapshot, as for dye.
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            return UseResult::Success;
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

            const glm::ivec3 clicked = ctx.hitResult.blockPos;

            // :56-79 the first branch: a Spawner block entity is reprogrammed
            // by the egg. With spawner_blocks_work off MC says so and FAILs;
            // otherwise setEntityId (the op-only ENTITY_DATA variant has no
            // component to read here), sendBlockUpdated, BLOCK_CHANGE, and
            // one egg is used (creative restores it through the dispatch's
            // stack snapshot, as below).
            if (auto* spawner = dynamic_cast<SpawnerBlockEntity*>(ctx.world->GetBlockEntity(clicked))) {
                if (!Rules::GetBool(Rules::Id::SpawnerBlocksWork)) {
                    if (ctx.player) {
                        ctx.player->DisplayClientMessage("Spawner blocks are disabled", /*actionBar=*/false);
                    }
                    return UseResult::Fail;
                }
                JavaRandom* random = ctx.world->Random();
                JavaRandom fallback(0);
                spawner->SetEntityId(type, random ? *random : fallback);
                ctx.world->BlockEntityChanged(clicked);
                GameEventEmit("block_change", clicked);
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
                return UseResult::Success;
            }

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
            // EntityType.createDefaultStackConfig → applyComponentsFromItemStack:
            // a renamed egg names the mob it spawns.
            std::optional<std::string> eggName = stack.components.get(DataComponents::CUSTOM_NAME);
            const auto applyStackComponents = [&eggName](Mob& mob) {
                if (eggName) mob.SetCustomName(eggName);
            };
            if (SpawnMobFromItem(type, spawnPos, /*tryMoveDown=*/true, movedUp,
                                 ctx.world->GetDimension(), /*portalCooldownTicks=*/0,
                                 applyStackComponents)) {
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
            // HoneycombItem.useOn:71 — playSound(player, pos, HONEYCOMB_WAX_ON,
            // BLOCKS, 1.0, 1.0).
            ctx.world->PlaySound(ctx.player, pos, SoundEvents::HONEYCOMB_WAX_ON, SoundSource::Blocks, 1.0f, 1.0f);
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
                        // TODO(particles): no particle system yet. The sound
                        // half is the server's (`if (!level.isClientSide())`),
                        // for everyone.
                        if (!ctx.world->IsClientSide()) {
                            PlayLevelEventSound(*ctx.world, nullptr, LevelEvent::PARTICLES_AND_SOUND_PLANT_GROWTH,
                                                pos, 15, ctx.world->Random());
                        }
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

            // :43 levelEvent 1505 (bone-meal particles — particle system TODO;
            // its sound, server-side, for everyone).
            if (!ctx.world->IsClientSide()) {
                PlayLevelEventSound(*ctx.world, nullptr, LevelEvent::PARTICLES_AND_SOUND_PLANT_GROWTH, pos, 15,
                                    ctx.world->Random());
            }
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
                // Aurelith's resonant water is a fluid cell here too (its
                // shape is empty, like a bubble column's, so the shape test
                // below would let the ray through it).
                const bool isFluid = (block == BlockID::Water || block == BlockID::Lava ||
                                      block == BlockID::ResonantWater);
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

        // Bucket of sulfur cube — MC MobBucketItem with Fluids.EMPTY: the POV
        // clip runs with ClipContext.Fluid.NONE, the cube is spawned in the
        // cell on the clicked face (BucketItem.use's `relative`), the bucket
        // empties (getEmptySuccessItem), and MobBucketItem.spawn hands the
        // new cube its bucket data (loadFromBucketTag + setFromBucket).
        UseResult Use_SulfurCubeBucket(ILevelWrite* world, IUsePlayer* player,
                                       uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/false);
            if (!hit) return UseResult::Pass;

            const auto data = stack.components.get(DataComponents::SULFUR_CUBE_BUCKET);
            const auto configure = [data](Mob& mob) {
                if (auto* cube = dynamic_cast<SulfurCube*>(&mob)) {
                    cube->LoadFromBucket(data.value_or(SulfurCubeBucketData{}));
                }
            };
            if (!SpawnMobFromItem(EntityTypeId::SulfurCube, hit->beforePos,
                                  /*tryMoveDown=*/false, /*movedUp=*/false,
                                  world->GetDimension(), 0, configure)) {
                return UseResult::Fail;
            }
            GameEventEmit("entity_place", hit->beforePos);
            if (!player->isCreative()) {
                stack = ItemStack(Items::Bucket, 1);
                player->markSlotDirty(player->handSlotIndex(hand));
            }
            return UseResult::Success;
        }

        // Empty bucket — BucketItem.java:43-74 (fill path).
        // ── EnderEye.use — mirrors EnderEyeItem.java:76-108 ─────────────────
        //
        // Throws the eye toward the nearest stronghold. Aiming at an
        // end_portal_frame instead PASSES, so that the useOn path (seating an
        // eye) wins whenever the player is looking at a frame — without that
        // branch, filling a portal would throw the eye away instead.
        UseResult Use_EnderEye(ILevelWrite* world, IUsePlayer* player,
                               uint32_t /*hand*/, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;

            // MC getPlayerPOVHitResult(level, player, ClipContext.Fluid.NONE)
            // — the same POV clip the bucket uses, minus the fluid stop.
            if (auto hit = BucketClip(world, *player, /*stopOnFluid=*/false)) {
                if (hit->block == BlockID::EndPortalFrame) return UseResult::Pass;
            }

            // MC guards the whole tail with `if (level instanceof ServerLevel)`
            // and still returns SUCCESS_SERVER — the client swings its arm and
            // waits for the entity to arrive on the wire. There is nothing to
            // predict: an entity the client invented would be a duplicate.
            if (world->IsClientSide()) return UseResult::Success;

            const glm::dvec3 from = player->getPosition() + glm::dvec3(0.0, 0.5, 0.0);
            if (!ThrowEnderEye(player->getDimensionId(), from, stack)) {
                // MC returns CONSUME with the stack UNTOUCHED when there is no
                // structure to point at — the eye is not spent on a world that
                // has nowhere to send it.
                return UseResult::Consume;
            }

            // EnderEyeItem.use:103 — at the player, NEUTRAL, pitch
            // lerp(nextFloat, 0.33, 0.5); server-side, for everyone.
            world->PlaySound(nullptr, player->getPosition(), SoundEvents::ENDER_EYE_LAUNCH, SoundSource::Neutral,
                             1.0f, 0.33f + LevelRandomFloat(world) * (0.5f - 0.33f));
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            return UseResult::Success;
        }

        // ── EnderPearl.use — mirrors EnderpearlItem.java ───────────────────
        //
        // Throw a ThrownEnderpearl from the player's rotation (power 1.5,
        // inaccuracy 1.0) and spend one. The ENDER_PEARL_THROW sound is the
        // stubbed sound system; the 1-second useCooldown from the item's
        // properties waits on a cooldown system (noted in DispatchUseItem) —
        // until then pearls can be thrown back-to-back.
        UseResult Use_EnderPearl(ILevelWrite* world, IUsePlayer* player,
                                 uint32_t /*hand*/, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;

            // MC guards the spawn with `if (level instanceof ServerLevel)`
            // and still returns success — the client swings and waits for
            // the entity on the wire (same shape as the ender eye above).
            if (world->IsClientSide()) return UseResult::Success;

            if (!ThrowEnderPearl(player->getDimensionId(), *player)) {
                return UseResult::Fail;
            }

            // EnderpearlItem.use:25 — playSound(null, player, ENDER_PEARL_THROW,
            // NEUTRAL, 0.5, 0.4 / (nextFloat * 0.4 + 0.8)).
            world->PlaySound(nullptr, player->getPosition(), SoundEvents::ENDER_PEARL_THROW, SoundSource::Neutral,
                             0.5f, 0.4f / (LevelRandomFloat(world) * 0.4f + 0.8f));
            // MC itemStack.consume(1, player) — creative keeps the pearl.
            if (!player->isCreative()) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
            return UseResult::Success;
        }

        // MC Player.playSound(sound, volume, pitch): at the player, in PLAYERS,
        // `except` the player itself — its own client (LocalPlayer.playSound)
        // plays it locally, the server sends it to everyone else.
        void PlayerPlaySound(ILevelWrite* world, IUsePlayer* player, std::string_view event,
                             float volume, float pitch) {
            world->PlaySound(player, player->getPosition(), event, SoundSource::Players, volume, pitch);
        }

        UseResult Use_EmptyBucket(ILevelWrite* world, IUsePlayer* player,
                                  uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            // The Aether's SkyrootBucketItem (Fluids.EMPTY) is this item with
            // two differences: it fills to the SKYROOT water bucket, and it
            // cannot hold lava (SkyrootBucketItem.use passes on anything but
            // water).
            const bool skyroot = stack.itemId == Items::SkyrootBucket;
            const ItemID filledWater = skyroot ? Items::SkyrootWaterBucket : Items::WaterBucket;
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
                // BucketItem.use:82 — bucketPickup.getPickupSound() through
                // player.playSound (the waterlogged block's is water's).
                PlayerPlaySound(world, player, SoundEvents::BUCKET_FILL, 1.0f, 1.0f);
                {
                    player->CreateFilledResult(stack, ItemStack(filledWater, 1));
                    player->markSlotDirty(player->handSlotIndex(hand));
                }
                return UseResult::Success;
            }

            // Aurelith's resonant water is a BucketPickup the way MC's bubble
            // column is (BubbleColumnBlock.pickupBlock: the cell becomes air,
            // the bucket fills with plain WATER): its fluid state is a water
            // source, so it takes the water path below unchanged. What the
            // bucket holds is ordinary water — the river's glow stays in the
            // river (docs/fluids.md).
            if (hit->block != BlockID::Water && hit->block != BlockID::Lava &&
                hit->block != BlockID::ResonantWater) {
                return UseResult::Pass;                                    // :93-94 (BLOCK hit → pass)
            }
            if (skyroot && hit->block == BlockID::Lava) return UseResult::Pass;

            // LiquidBlock.pickupBlock: only a SOURCE (`level == 0`) fills the
            // bucket; flowing water hands back nothing. The clip above stops
            // on sources only (ClipContext.Fluid.SOURCE_ONLY), so a flowing
            // cell is never even the hit — this is the belt to that brace.
            if (!FluidStateOf(hitState).IsSource()) {
                return UseResult::Fail;
            }

            // :55-74 — pickupBlock writes air with flag 11, sound, transform
            // bucket → filled variant. Removing the source is what lets the
            // pool's neighbours drain: flag 1 tells them, and their
            // LiquidBlock.neighborChanged books the tick.
            world->SetBlock(hit->pos.x, hit->pos.y, hit->pos.z, BlockID::Air,
                            World::UpdateFlags::AllImmediate);
            // BucketItem.use:82 — the fluid's pickup sound, player.playSound.
            PlayerPlaySound(world, player,
                            hit->block == BlockID::Lava ? SoundEvents::BUCKET_FILL_LAVA : SoundEvents::BUCKET_FILL,
                            1.0f, 1.0f);
            // ItemUtils.createFilledResult (:62): creative keeps the empty
            // bucket, survival transforms it. Component patch reset — a
            // fresh filled bucket carries no per-stack state.
            // ItemUtils.createFilledResult: one bucket of the stack fills,
            // the rest stays (creative keeps the stack and gains the filled
            // bucket once).
            player->CreateFilledResult(stack, ItemStack(hit->block == BlockID::Lava ? Items::LavaBucket
                                                                                      : filledWater, 1));
            player->markSlotDirty(player->handSlotIndex(hand));
            return UseResult::Success;
        }

        // Glass bottle — MC BottleItem.use: fill from WATER at the POV clip
        // (ClipContext.Fluid.SOURCE_ONLY) into a water bottle,
        // PotionContents.createItemStack(POTION, WATER), through
        // ItemUtils.createFilledResult. A waterlogged block counts — its
        // fluid state is a water source. MC's first branch (an ender
        // dragon's breath cloud within 2 blocks → dragon's breath) needs the
        // dragon to OWN its breath clouds, which the engine's dragon does not
        // record; that branch is left out.
        UseResult Use_GlassBottle(ILevelWrite* world, IUsePlayer* player,
                                  uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/true);
            if (!hit) return UseResult::Pass;
            const BlockState state = world->GetBlockState(hit->pos.x, hit->pos.y, hit->pos.z);
            if (!FluidStateOf(state).IsSourceOf(FluidType::Water)) return UseResult::Pass;
            // BottleItem.use:59 — playSound(player, player's position,
            // BOTTLE_FILL, NEUTRAL, 1.0, 1.0).
            world->PlaySound(player, player->getPosition(), SoundEvents::BOTTLE_FILL, SoundSource::Neutral,
                             1.0f, 1.0f);
            // GameEvent.FLUID_PICKUP — no game-event system.
            player->CreateFilledResult(stack, CreatePotionItemStack(Items::Potion, PotionId::Water));
            player->markSlotDirty(player->handSlotIndex(hand));
            return UseResult::Success;
        }

        // Filled bucket — BucketItem.java:76-95 + emptyContents (:104-179,
        // simplified: no fluid simulation, sources are static cubes).
        UseResult Use_FilledBucket(ILevelWrite* world, IUsePlayer* player,
                                   uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            const bool isLava = (stack.itemId == Items::LavaBucket);
            // SkyrootBucketItem(Fluids.WATER): pours like the water bucket and
            // leaves its own empty skyroot bucket behind.
            const ItemID emptied = stack.itemId == Items::SkyrootWaterBucket ? Items::SkyrootBucket
                                                                             : Items::Bucket;
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/false);  // ClipContext.Fluid.NONE
            if (!hit) return UseResult::Pass;

            // The Aether's ignition — DimensionHooks.createPortal, which the
            // mod runs from PlayerInteractEvent.RightClickBlock, i.e. BEFORE
            // BucketItem.use gets to pour: water against a glowstone frame
            // lights an empty frame around the cell past the clicked face
            // (either axis), and the bucket empties into the portal instead
            // of the world. The client only predicts (TryLight writes
            // nothing there), so it does not pour a source the server will
            // not have. `#aether:aether_portal_activation_items` is the
            // water bucket alone.
            if (stack.itemId == Items::WaterBucket && hit->block == BlockID::Glowstone &&
                AetherPortalIgnition::TryLight(*world, hit->beforePos)) {
                world->PlaySound(player, hit->beforePos, SoundEvents::BUCKET_EMPTY, SoundSource::Blocks, 1.0f, 1.0f);
                // Not creative: the stack's crafting remainder, the empty
                // bucket (the mod's setItemInHand(getCraftingRemainingItem)).
                if (!player->isCreative()) {
                    stack = ItemStack(emptied, 1);
                    player->markSlotDirty(player->handSlotIndex(hand));
                }
                return UseResult::Success;
            }

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
                // SimpleWaterloggedBlock.placeLiquid's second half: the new
                // water books its first tick so it can start flowing out of
                // the block. A no-op on the client (no scheduler).
                Fluids::ScheduleTick(*world, hit->pos, FluidType::Water);
                // BucketItem.emptyContents → playEmptySound(user, level, pos).
                world->PlaySound(player, hit->pos, SoundEvents::BUCKET_EMPTY, SoundSource::Blocks, 1.0f, 1.0f);
                if (!player->isCreative()) {
                    stack = ItemStack(emptied, 1);
                    player->markSlotDirty(player->handSlotIndex(hand));
                }
                return UseResult::Success;
            }

            // BucketItem.use:84 — the cell in front of the clicked face
            // (pos.relative(direction)); the clicked cell itself only for
            // water into a LiquidBlockContainer, handled above.
            glm::ivec3 target = hit->beforePos;
            if (!world->IsValidPosition(target.x, target.y, target.z)) {
                return UseResult::Fail;
            }
            const BlockState targetState =
                world->GetBlockState(target.x, target.y, target.z);
            const BlockID targetBlock = targetState.Block();

            // emptyContents: `mayReplace = blockState.canBeReplaced(content)`
            // — BlockBehaviour.canBeReplaced(state, fluid) is
            // `canBeReplaced() || !isSolid()`: the replaceable flag (water,
            // lava, tall grass, snow layers, fire…) or no collision at all
            // (a torch, a flower). `canPlaceFluidInsideBlock = isAir ||
            // mayReplace` — the shift-key term only matters on the first of
            // MC's two attempts, and the second retries without it, so the
            // net rule is exactly this.
            const bool mayReplace = BlockRegistry::Get(targetBlock).replaceable ||
                                    !BlockRegistry::HasCollision(targetBlock);
            if (targetBlock != BlockID::Air && !mayReplace) {
                return UseResult::Fail;                                     // :88
            }

            // EnvironmentAttributes.WATER_EVAPORATES (the nether's ultrawarm
            // flag): water poured in the nether hisses away and the bucket
            // still empties. MC also spawns eight LARGE_SMOKE here — no
            // server→client particle channel yet.
            if (!isLava && world->GetDimension() == DimensionId::Nether) {
                float pitch = 2.6f;
                if (JavaRandom* random = world->Random()) {
                    pitch += (random->NextFloat() - random->NextFloat()) * 0.8f;
                }
                // BucketItem.emptyContents:154 — playSound(user, pos, FIRE_EXTINGUISH, …).
                world->PlaySound(player, target, SoundEvents::FIRE_EXTINGUISH, SoundSource::Blocks, 0.5f, pitch);
                if (!player->isCreative()) {
                    stack = ItemStack(emptied, 1);
                    player->markSlotDirty(player->handSlotIndex(hand));
                }
                return UseResult::Success;
            }

            // `if (!isClientSide && mayReplace && !blockState.liquid())
            //     level.destroyBlock(pos, true)` — the tall grass the water
            // displaces drops as an item.
            const bool targetIsLiquid = targetBlock == BlockID::Water || targetBlock == BlockID::Lava;
            if (!world->IsClientSide() && mayReplace && !targetIsLiquid &&
                targetBlock != BlockID::Air) {
                world->DestroyBlock(target, true);
            }

            // `level.setBlock(pos, content.defaultFluidState().createLegacyBlock(), 11)`
            // — a SOURCE, flag 11. A write that changes nothing (pouring into
            // an identical source) still counts as success in vanilla; only
            // a no-op over a non-source fails.
            const bool wrote = world->SetBlock(target.x, target.y, target.z,
                                               isLava ? BlockID::Lava : BlockID::Water,
                                               World::UpdateFlags::AllImmediate);
            if (!wrote && !FluidStateOf(targetState).IsSource()) {
                return UseResult::Fail;
            }
            // BucketItem.playEmptySound:188 — (user, pos, BUCKET_EMPTY(_LAVA), BLOCKS).
            world->PlaySound(player, target, isLava ? SoundEvents::BUCKET_EMPTY_LAVA : SoundEvents::BUCKET_EMPTY,
                             SoundSource::Blocks, 1.0f, 1.0f);                // :175-179
            // :97-99 getEmptySuccessItem — creative keeps the filled bucket.
            if (!player->isCreative()) {
                stack = ItemStack(emptied, 1);
                player->markSlotDirty(player->handSlotIndex(hand));
            }
            return UseResult::Success;                                      // :90
        }

        // ── The Hush: the tools of the deep (docs/the-hush.md) ────────────
        //
        // Thin shells over the server bridges in common/world/level/
        // HushItems.hpp: every one of these items does its work on the
        // server (a lookup, a packet, a teleport, an arrow). The client's run
        // of the same dispatch only needs the right UseResult — a swing, or
        // "not a placement" — so it answers without calling across.

        // Tuning fork, used on a resonant crystal or cluster: the ping. Any
        // other block is not the fork's business (PASS, the ender-eye rule).
        UseResult UseOn_TuningFork(const UseOnContext& ctx, ItemStack& stack) {
            (void)stack;
            if (!ctx.world || !ctx.player) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockID target = ctx.world->GetBlockState(pos.x, pos.y, pos.z).Block();
            if (target != BlockID::ResonantCrystal && target != BlockID::ResonantCluster) {
                return UseResult::Pass;
            }
            if (ctx.world->IsClientSide()) return UseResult::Success;
            return HushItems::TuningForkStrike(*ctx.player, pos);
        }

        // MC BowItem.use: draw if there is an arrow (or creative). The vanilla
        // bow and the resonance bow share it; the release (BowRelease) picks
        // the arrow kind from the bow.
        UseResult Use_Bow(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                          ItemStack& /*stack*/) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Consume;
            return HushItems::BowBegin(*player, hand);
        }

        // Recall chime: start the hold (kRecallUseTicks) if there is a gate
        // to go back to and the chime is not still ringing; the teleport is
        // the finish.
        UseResult Use_RecallChime(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                                  ItemStack& /*stack*/) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Consume;
            return HushItems::RecallChimeBegin(*player, hand);
        }

        // The Held Note (Aurelith's reward): a 1 s hold, then the Chord
        // sounds and the hostile mobs round the player stop (server:
        // server/items/AurelithItems.cpp). Not used up.
        UseResult Use_HeldNote(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                               ItemStack& /*stack*/) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Consume;
            return Aurelith::HeldNoteBegin(*player, hand) ? UseResult::Consume : UseResult::Fail;
        }

        void Finish_HeldNote(IUsePlayer& player, ItemStack& /*stack*/) {
            Aurelith::HeldNoteFinish(player);
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

        // EnderEye — seats an eye in an end_portal_frame and opens the portal
        // once the ring is complete. Wiring a useOn also takes the eye out of
        // the client's block-placement prediction (PlayerController refuses to
        // predict placement for any item that has one), which is what stops it
        // predicting a block that is not a block item at all.
        wireUseOn(Items::EnderEye, &UseOn_EnderEye);

        // EchoShard — lights a reinforced-deepslate frame as a Hush portal
        // (PortalFamily::Hush). Same prediction note as the eye: a useOn
        // keeps the client from predicting a block placement for it.
        wireUseOn(Items::EchoShard, &UseOn_EchoShard);

        // ── The Hush: the tools of the deep (docs/the-hush.md) ───────────
        // Tuning fork: the resonance ping, struck on a crystal.
        wireUseOn(Items::TuningFork, &UseOn_TuningFork);
        {
            auto setHold = [&](ItemID id, ItemUseFn use, int duration, ItemUseAnimation anim,
                               ItemFinishUsingFn finish, ItemReleaseUsingFn release) {
                auto it = pureItems.find(id);
                if (it == pureItems.end()) return;
                it->second.use          = use;
                it->second.useDuration  = duration;
                it->second.useAnimation = anim;
                it->second.finishUsing  = finish;
                it->second.releaseUsing = release;
            };
            // MC BowItem (getUseDuration 72000, BOW, fires on release). This
            // engine had no bow at all: the resonance bow needed the port, so
            // the vanilla bow gets it too — plain arrows from one, resonance
            // arrows from the other (HushItems::BowRelease).
            setHold(Items::Bow,          &Use_Bow, 72000, ItemUseAnimation::BOW,
                    nullptr, &HushItems::BowRelease);
            setHold(Items::ResonanceBow, &Use_Bow, 72000, ItemUseAnimation::BOW,
                    nullptr, &HushItems::BowRelease);
            // Recall chime: a hold as long as its note (the goat horn's
            // pose), then home to the last hush gate; letting go early only
            // says so. The chime is not used up.
            setHold(Items::RecallChime, &Use_RecallChime, HushItems::kRecallUseTicks,
                    ItemUseAnimation::TOOT_HORN, &HushItems::RecallChimeFinish,
                    &HushItems::RecallChimeRelease);
            // The Held Note: a second's hold in the horn's pose, then the Chord.
            setHold(Items::HeldNote, &Use_HeldNote, Aurelith::kHeldNoteUseTicks,
                    ItemUseAnimation::TOOT_HORN, &Finish_HeldNote, nullptr);
        }
        // The whisperfruit plants its block under a lantern leaf (the sweet
        // berries' placesBlock; CanSurviveAt carries the leaf rule).
        wirePlacesBlock(Items::Whisperfruit, BlockID::HangingWhisperfruit);

        // Every hoe tier shares the till behaviour. Tool material (mining
        // speed, durability, attack damage) is a per-item property MC reads
        // from the Item.Properties.hoe(material, …) builder; we don't model
        // tool materials yet, so all tiers behave identically until we do.
        for (ItemID id : {
                Items::WoodenHoe, Items::CopperHoe, Items::StoneHoe,
                Items::GoldenHoe, Items::IronHoe,   Items::DiamondHoe,
                Items::NetheriteHoe, Items::ResoniteHoe,
                // the Aether's and Twilight Forest's tiers (docs/mod-ports.md)
                Items::SkyrootHoe, Items::HolystoneHoe, Items::ZaniteHoe, Items::GravititeHoe, Items::IronwoodHoe, Items::SteeleafHoe }) {
            wireUseOn(id, &UseOn_Hoe);
        }

        // Same for every shovel tier.
        for (ItemID id : {
                Items::WoodenShovel, Items::CopperShovel, Items::StoneShovel,
                Items::GoldenShovel, Items::IronShovel,   Items::DiamondShovel,
                Items::NetheriteShovel, Items::ResoniteShovel,
                // the Aether's and Twilight Forest's tiers (docs/mod-ports.md)
                Items::SkyrootShovel, Items::HolystoneShovel, Items::ZaniteShovel, Items::GravititeShovel, Items::IronwoodShovel, Items::SteeleafShovel }) {
            wireUseOn(id, &UseOn_Shovel);
        }

        // Every axe tier shares strip/scrape/wax-off (AxeItem.java:38-105).
        for (ItemID id : {
                Items::WoodenAxe, Items::CopperAxe, Items::StoneAxe,
                Items::GoldenAxe, Items::IronAxe,   Items::DiamondAxe,
                Items::NetheriteAxe, Items::ResoniteAxe,
                // the Aether's and Twilight Forest's tiers (docs/mod-ports.md)
                Items::SkyrootAxe, Items::HolystoneAxe, Items::ZaniteAxe, Items::GravititeAxe, Items::IronwoodAxe, Items::SteeleafAxe, Items::KnightmetalAxe }) {
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

        // Name tag (NameTagItem.java). Mob.checkAndHandleImportantInteractions
        // gives it the click before the mob's own interaction — see
        // IntegratedServer::HandleInteract.
        wireInteract(Items::NameTag, &InteractEntity_NameTag);

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
        wireUse(Items::EnderEye,    &Use_EnderEye);
        wireUse(Items::EnderPearl,  &Use_EnderPearl);
        wireUse(Items::Bucket,      &Use_EmptyBucket);
        wireUse(Items::GlassBottle, &Use_GlassBottle);
        wireUse(Items::SulfurCubeBucket, &Use_SulfurCubeBucket);
        wireUse(Items::WaterBucket, &Use_FilledBucket);
        wireUse(Items::LavaBucket,  &Use_FilledBucket);
        // The Aether's skyroot buckets (SkyrootBucketItem): the same use, their
        // own empty/filled pair (see Use_EmptyBucket / Use_FilledBucket).
        wireUse(Items::SkyrootBucket,      &Use_EmptyBucket);
        wireUse(Items::SkyrootWaterBucket, &Use_FilledBucket);
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
        // remain useful. Resonite (The Hush, docs/the-hush.md) is netherite's
        // twin in speed and mining level, and additionally the only tier that
        // opens the echo core (MiningTier.hpp).
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
        setTool(Items::ResonitePickaxe,  Tool{ToolType::Pickaxe, MiningTier::Resonite,  9.0f});
        // Axes
        setTool(Items::WoodenAxe,        Tool{ToolType::Axe,     MiningTier::Wood,      2.0f});
        setTool(Items::CopperAxe,        Tool{ToolType::Axe,     MiningTier::Stone,     5.0f});
        setTool(Items::StoneAxe,         Tool{ToolType::Axe,     MiningTier::Stone,     4.0f});
        setTool(Items::GoldenAxe,        Tool{ToolType::Axe,     MiningTier::Gold,     12.0f});
        setTool(Items::IronAxe,          Tool{ToolType::Axe,     MiningTier::Iron,      6.0f});
        setTool(Items::DiamondAxe,       Tool{ToolType::Axe,     MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteAxe,     Tool{ToolType::Axe,     MiningTier::Netherite, 9.0f});
        setTool(Items::ResoniteAxe,      Tool{ToolType::Axe,     MiningTier::Resonite,  9.0f});
        // Shovels
        setTool(Items::WoodenShovel,     Tool{ToolType::Shovel,  MiningTier::Wood,      2.0f});
        setTool(Items::CopperShovel,     Tool{ToolType::Shovel,  MiningTier::Stone,     5.0f});
        setTool(Items::StoneShovel,      Tool{ToolType::Shovel,  MiningTier::Stone,     4.0f});
        setTool(Items::GoldenShovel,     Tool{ToolType::Shovel,  MiningTier::Gold,     12.0f});
        setTool(Items::IronShovel,       Tool{ToolType::Shovel,  MiningTier::Iron,      6.0f});
        setTool(Items::DiamondShovel,    Tool{ToolType::Shovel,  MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteShovel,  Tool{ToolType::Shovel,  MiningTier::Netherite, 9.0f});
        setTool(Items::ResoniteShovel,   Tool{ToolType::Shovel,  MiningTier::Resonite,  9.0f});
        // Hoes
        setTool(Items::WoodenHoe,        Tool{ToolType::Hoe,     MiningTier::Wood,      2.0f});
        setTool(Items::CopperHoe,        Tool{ToolType::Hoe,     MiningTier::Stone,     5.0f});
        setTool(Items::StoneHoe,         Tool{ToolType::Hoe,     MiningTier::Stone,     4.0f});
        setTool(Items::GoldenHoe,        Tool{ToolType::Hoe,     MiningTier::Gold,     12.0f});
        setTool(Items::IronHoe,          Tool{ToolType::Hoe,     MiningTier::Iron,      6.0f});
        setTool(Items::DiamondHoe,       Tool{ToolType::Hoe,     MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteHoe,     Tool{ToolType::Hoe,     MiningTier::Netherite, 9.0f});
        setTool(Items::ResoniteHoe,      Tool{ToolType::Hoe,     MiningTier::Resonite,  9.0f});
        // Swords (used for cobweb / bamboo speedup in MC)
        setTool(Items::WoodenSword,      Tool{ToolType::Sword,   MiningTier::Wood,      2.0f});
        setTool(Items::CopperSword,      Tool{ToolType::Sword,   MiningTier::Stone,     5.0f});
        setTool(Items::StoneSword,       Tool{ToolType::Sword,   MiningTier::Stone,     4.0f});
        setTool(Items::GoldenSword,      Tool{ToolType::Sword,   MiningTier::Gold,     12.0f});
        setTool(Items::IronSword,        Tool{ToolType::Sword,   MiningTier::Iron,      6.0f});
        setTool(Items::DiamondSword,     Tool{ToolType::Sword,   MiningTier::Diamond,   8.0f});
        setTool(Items::NetheriteSword,   Tool{ToolType::Sword,   MiningTier::Netherite, 9.0f});
        setTool(Items::ResoniteSword,    Tool{ToolType::Sword,   MiningTier::Resonite,  9.0f});
        // Shears (single tier; MC speed = 1.5 against most, 15.0 vs wool/leaves)
        // No useOn wired: ShearsItem's interactions (beehive honeycombs,
        // pumpkin carving) all produce item DROPS — BLOCKED on item entities.
        setTool(Items::Shears,           Tool{ToolType::Shears,  MiningTier::Iron,      1.5f});

        // The Aether (AetherItemTiers) and Twilight Forest (TFToolMaterials)
        // tool sets, docs/mod-ports.md. Their incorrect-for-drops tags are the
        // vanilla tiers' (skyroot = wooden, holystone = stone, zanite = iron,
        // gravitite = diamond; ironwood = iron, steeleaf / knightmetal =
        // diamond, fiery = netherite), so they need no new MiningTier; the
        // speed is the material's own. The steeleaf sword is registered on
        // KNIGHTMETAL in TFItems, which has the same speed.
        setTool(Items::SkyrootPickaxe,        Tool{ToolType::Pickaxe, MiningTier::Wood,      2.0f});
        setTool(Items::SkyrootAxe,            Tool{ToolType::Axe,     MiningTier::Wood,      2.0f});
        setTool(Items::SkyrootShovel,         Tool{ToolType::Shovel,  MiningTier::Wood,      2.0f});
        setTool(Items::SkyrootHoe,            Tool{ToolType::Hoe,     MiningTier::Wood,      2.0f});
        setTool(Items::SkyrootSword,          Tool{ToolType::Sword,   MiningTier::Wood,      2.0f});
        setTool(Items::HolystonePickaxe,      Tool{ToolType::Pickaxe, MiningTier::Stone,     4.0f});
        setTool(Items::HolystoneAxe,          Tool{ToolType::Axe,     MiningTier::Stone,     4.0f});
        setTool(Items::HolystoneShovel,       Tool{ToolType::Shovel,  MiningTier::Stone,     4.0f});
        setTool(Items::HolystoneHoe,          Tool{ToolType::Hoe,     MiningTier::Stone,     4.0f});
        setTool(Items::HolystoneSword,        Tool{ToolType::Sword,   MiningTier::Stone,     4.0f});
        setTool(Items::ZanitePickaxe,         Tool{ToolType::Pickaxe, MiningTier::Iron,      6.0f});
        setTool(Items::ZaniteAxe,             Tool{ToolType::Axe,     MiningTier::Iron,      6.0f});
        setTool(Items::ZaniteShovel,          Tool{ToolType::Shovel,  MiningTier::Iron,      6.0f});
        setTool(Items::ZaniteHoe,             Tool{ToolType::Hoe,     MiningTier::Iron,      6.0f});
        setTool(Items::ZaniteSword,           Tool{ToolType::Sword,   MiningTier::Iron,      6.0f});
        setTool(Items::GravititePickaxe,      Tool{ToolType::Pickaxe, MiningTier::Diamond,   8.0f});
        setTool(Items::GravititeAxe,          Tool{ToolType::Axe,     MiningTier::Diamond,   8.0f});
        setTool(Items::GravititeShovel,       Tool{ToolType::Shovel,  MiningTier::Diamond,   8.0f});
        setTool(Items::GravititeHoe,          Tool{ToolType::Hoe,     MiningTier::Diamond,   8.0f});
        setTool(Items::GravititeSword,        Tool{ToolType::Sword,   MiningTier::Diamond,   8.0f});
        setTool(Items::IronwoodPickaxe,       Tool{ToolType::Pickaxe, MiningTier::Iron,      6.5f});
        setTool(Items::IronwoodAxe,           Tool{ToolType::Axe,     MiningTier::Iron,      6.5f});
        setTool(Items::IronwoodShovel,        Tool{ToolType::Shovel,  MiningTier::Iron,      6.5f});
        setTool(Items::IronwoodHoe,           Tool{ToolType::Hoe,     MiningTier::Iron,      6.5f});
        setTool(Items::IronwoodSword,         Tool{ToolType::Sword,   MiningTier::Iron,      6.5f});
        setTool(Items::SteeleafPickaxe,       Tool{ToolType::Pickaxe, MiningTier::Diamond,   8.0f});
        setTool(Items::SteeleafAxe,           Tool{ToolType::Axe,     MiningTier::Diamond,   8.0f});
        setTool(Items::SteeleafShovel,        Tool{ToolType::Shovel,  MiningTier::Diamond,   8.0f});
        setTool(Items::SteeleafHoe,           Tool{ToolType::Hoe,     MiningTier::Diamond,   8.0f});
        setTool(Items::SteeleafSword,         Tool{ToolType::Sword,   MiningTier::Diamond,   8.0f});
        setTool(Items::KnightmetalPickaxe,    Tool{ToolType::Pickaxe, MiningTier::Diamond,   8.0f});
        setTool(Items::KnightmetalAxe,        Tool{ToolType::Axe,     MiningTier::Diamond,   8.0f});
        setTool(Items::KnightmetalSword,      Tool{ToolType::Sword,   MiningTier::Diamond,   8.0f});
        setTool(Items::FieryPickaxe,          Tool{ToolType::Pickaxe, MiningTier::Netherite, 9.0f});
        setTool(Items::FierySword,            Tool{ToolType::Sword,   MiningTier::Netherite, 9.0f});

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
            // The Hush (docs/the-hush.md): the boss drop and the capstone
            // sword sit with the mace and elytra.
            setRarity(Items::ResonantHeart,        Rarity::EPIC);
            setRarity(Items::EchoBlade,            Rarity::EPIC);
            setRarity(Items::ChoirHeart,           Rarity::EPIC);   // the Choir Mother's drop
            // The tools of the deep: the capstone-adjacent bow and cloak sit
            // with the recovery compass; the rest are workaday.
            setRarity(Items::ResonanceBow,         Rarity::UNCOMMON);
            setRarity(Items::CloakOfSilence,       Rarity::UNCOMMON);
            setRarity(Items::EchoCompass,          Rarity::UNCOMMON);
            // Aurelith, reawakening the Heart: the four voice keys are the
            // city's treasures (RARE, the nether star's tier); the Held
            // Note, sung out of the Heart itself, sits with the boss drops.
            setRarity(Items::SopranoVoiceKey,      Rarity::RARE);
            setRarity(Items::AltoVoiceKey,         Rarity::RARE);
            setRarity(Items::TenorVoiceKey,        Rarity::RARE);
            setRarity(Items::BassVoiceKey,         Rarity::RARE);
            setRarity(Items::HeldNote,             Rarity::EPIC);
        }

        // Food/consumable component defaults (FoodDefs.cpp).
        ItemRegistry_RegisterFoods(pureItems);

        // Armor/shield equipment defaults (EquipmentBehavior.cpp).
        ItemRegistry_RegisterEquipment(pureItems);

        // Bundle click behaviours + crafting remainders (BundleBehavior.cpp).
        ItemRegistry_RegisterBundles(pureItems);

        // Potions, splash/lingering throws, tipped arrows, suspicious stew
        // (alchemy/PotionItems.cpp).
        ItemRegistry_RegisterPotionItems(pureItems);

        // Book and quill / written book (BookItems.cpp).
        ItemRegistry_RegisterBookItems(pureItems);

        Log::Info("[ItemRegistry] Wired use-behaviour callbacks "
                  "(FlintAndSteel, 8 hoes, 8 shovels) + Tool components on 41 tool items");
    }

} // namespace Game
