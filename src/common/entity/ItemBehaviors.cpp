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

#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/entity/SpearItem.hpp"
#include "common/entity/WeaponItems.hpp"
#include "common/entity/vehicle/VehicleEntity.hpp"
#include "Item.hpp"
#include "common/particle/LevelParticles.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "GeneratedItemList.hpp"
#include "SpawnEggs.hpp"
#include "common/entity/decoration/HangingEntity.hpp"
#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "EndCrystal.hpp"
#include "FallingBlockEntity.hpp"
#include "PrimedTnt.hpp"
#include "projectile/Projectile.hpp"
#include "mobs/SulfurCube.hpp"
#include "mobs/Fish.hpp"
#include "mobs/AnimatedMobs.hpp"
#include "Bucketable.hpp"
#include "../world/level/WorldMobSpawn.hpp"
#include "mobs/Animals.hpp"
#include "ArmorStand.hpp"
#include "../data/DataComponents.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../world/block/BlockPlacement.hpp"
#include "../world/block/CandleBlocks.hpp"
#include "../world/block/CopperChestBlock.hpp"
#include "../world/fluid/FlowingFluid.hpp"
#include "../world/level/DimensionId.hpp"
#include "../world/level/World.hpp"
#include "../world/level/WorldDrops.hpp"
#include "../world/level/HushItems.hpp"
#include "../world/map/MapItem.hpp"
#include "FoodOnAStickItem.hpp"
#include "../world/level/AurelithQuest.hpp"
#include "../world/portal/ModPortalBehaviors.hpp"
#include "../world/portal/PortalFamily.hpp"
#include "../world/portal/PortalShape.hpp"
#include "../world/portal/PortalState.hpp"
#include "server/advancements/CriteriaTriggers.hpp"
#include "../world/portal/EndPortalFrame.hpp"
#include "../core/JavaRandom.hpp"
#include "../core/Mth.hpp"
#include "../core/Log.hpp"
#include "IUsePlayer.hpp"
#include "projectile/FishingHook.hpp"
#include "../world/block/entity/SpawnerBlockEntity.hpp"
#include "../world/block/entity/TrialSpawnerBlockEntity.hpp"
#include "../world/level/GameRules.hpp"
#include "ArchaeologyItems.hpp"
#include "Instruments.hpp"

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
    void ItemRegistry_RegisterTrialItems(std::unordered_map<ItemID, Item>& pureItems);  // TrialItems.cpp
    void ItemRegistry_RegisterVehicleItems(std::unordered_map<ItemID, Item>& pureItems); // vehicle/VehicleItems.cpp

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

        // MC `level.gameEvent(player, event, pos)` — Context.of(player). The
        // player is the server's entity for them (IUsePlayer::
        // GameEventSource); on the client's prediction the level has no
        // dispatcher and this is a no-op, as ClientLevel.gameEvent is.
        void GameEventEmit(ILevelWrite* level, IUsePlayer* player, GameEventId event, const glm::ivec3& pos) {
            if (!level) return;
            level->GameEvent(player ? player->GameEventSource() : nullptr, event, pos);
        }

        // MC `level.gameEvent(event, pos, Context.of(player, state))` — with
        // the affected state; `state` defaults to what now sits at `pos`.
        void GameEventEmitState(ILevelWrite* level, IUsePlayer* player, GameEventId event, const glm::ivec3& pos,
                                std::optional<BlockState> state = std::nullopt) {
            if (!level) return;
            const BlockState s = state ? *state : level->GetBlockState(pos.x, pos.y, pos.z);
            level->GameEvent(event, pos, GameEventContext::Of(player ? player->GameEventSource() : nullptr, s));
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
            // CandleBlock.canLight / CandleCakeBlock.canLight: the same relight
            // branch, `state.setValue(LIT, true)` with flags 11.
            {
                const BlockState cur = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
                if (Candles::CanLight(cur)) {
                    ctx.world->PlaySound(ctx.player, pos, SoundEvents::FLINTANDSTEEL_USE, SoundSource::Blocks,
                                         1.0f, LevelRandomFloat(ctx.world) * 0.4f + 0.8f);
                    if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, Candles::WithLit(cur, true),
                                             World::UpdateFlags::All)) {
                        return UseResult::Fail;
                    }
                    // FlintAndSteelItem.useOn:53 — gameEvent(player, BLOCK_CHANGE, pos).
                    GameEventEmit(ctx.world, ctx.player, GameEventId::BlockChange, pos);
                    UseOnHurtAndBreak(stack, 1, ctx);
                    return UseResult::Success;
                }
            }
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
                    // FlintAndSteelItem.useOn:53 — gameEvent(player, BLOCK_CHANGE, pos).
                    GameEventEmit(ctx.world, ctx.player, GameEventId::BlockChange, pos);
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
            GameEventEmit(ctx.world, ctx.player, GameEventId::BlockPlace, pos);

            // MC: `CriteriaTriggers.PLACED_BLOCK.trigger(serverPlayer, firePos, itemStack)`.
            if (!ctx.world->IsClientSide()) {
                if (Server::ServerPlayer* sp = Server::CriteriaTriggers::PlayerOf(ctx.player)) {
                    Server::CriteriaTriggers::PlacedBlock(*sp, firePos, stack);
                }
            }

            // MC: `if (player instanceof ServerPlayer) itemStack.hurtAndBreak(1, player, hand.asEquipmentSlot());`
            UseOnHurtAndBreak(stack, 1, ctx);

            return UseResult::Success;
        }

        // ── FireChargeItem.useOn — mirrors FireChargeItem.java:31-57 ────────
        //
        // Flint and steel's twin with two differences: the charge is used up
        // rather than worn, and its sound is FIRECHARGE_USE for everyone
        // (`playSound(null, …)`), pitched (r - r) * 0.2 + 1.
        UseResult UseOn_FireCharge(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            ILevelWrite& level = *ctx.world;
            glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);

            auto playSound = [&level](const glm::ivec3& at) {
                JavaRandom* r = level.Random();
                const float a = r ? r->NextFloat() : 0.5f;
                const float b = r ? r->NextFloat() : 0.5f;
                level.PlaySound(nullptr, at, SoundEvents::FIRECHARGE_USE, SoundSource::Blocks,
                                1.0f, (a - b) * 0.2f + 1.0f);
            };

            // CampfireBlock.canLight: a #campfires block with LIT and
            // WATERLOGGED, neither set.
            const bool campfireCanLight =
                (state.Is(BlockID::Campfire) || state.Is(BlockID::SoulCampfire)) &&
                state.HasProperty(PropertyId::LIT) && state.HasProperty(PropertyId::WATERLOGGED) &&
                state.GetName(PropertyId::LIT) == "false" &&
                state.GetName(PropertyId::WATERLOGGED) == "false";

            bool used = false;
            if (!campfireCanLight && !Candles::CanLight(state)) {
                // pos = pos.relative(clickedFace); canBePlacedAt(level, pos,
                // context.getHorizontalDirection()) — the LOOK direction picks
                // the portal axis here, where flint and steel passes the face.
                pos = ctx.getPlacementPos();
                if (CanFireBePlacedAt(ctx.world, pos, static_cast<int>(ctx.getHorizontalDirection()))) {
                    playSound(pos);
                    level.SetBlock(pos.x, pos.y, pos.z, Fluids::FireStateFor(level, pos),
                                   World::UpdateFlags::All);
                    // FireChargeItem.useOn:41 — gameEvent(player, BLOCK_PLACE, pos).
                    GameEventEmit(ctx.world, ctx.player, GameEventId::BlockPlace, pos);
                    used = true;
                }
            } else {
                playSound(pos);
                level.SetBlock(pos.x, pos.y, pos.z, state.SetName(PropertyId::LIT, "true"),
                               World::UpdateFlags::All);
                // FireChargeItem.useOn:47 — gameEvent(player, BLOCK_CHANGE, pos).
                GameEventEmit(ctx.world, ctx.player, GameEventId::BlockChange, pos);
                used = true;
            }

            if (!used) return UseResult::Fail;
            // context.getItemInHand().shrink(1) — creative is restored by the
            // dispatch's whole-stack snapshot, as for bone meal.
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
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
            // MC Block.pushEntitiesUp(targetState, newState, level, pos): the
            // eye ADDS EndPortalFrameBlock.SHAPE_EYE (column 8 wide, y 13..16
            // px) to the frame's shape, and every entity overlapping that is
            // lifted onto its top. (Players are the client's; their own
            // collision resolve lifts them.)
            if (EntityLevel* entities = ctx.world->Entities()) {
                const glm::vec3 lo(static_cast<float>(pos.x) + 0.25f, static_cast<float>(pos.y) + 0.8125f,
                                   static_cast<float>(pos.z) + 0.25f);
                const glm::vec3 hi(static_cast<float>(pos.x) + 0.75f, static_cast<float>(pos.y) + 1.0f,
                                   static_cast<float>(pos.z) + 0.75f);
                std::vector<Entity*> colliding;
                entities->GetEntitiesInBox(AABB::FromMinMax(lo, hi), nullptr, colliding);
                const double top = static_cast<double>(pos.y) + 1.0;
                for (Entity* entity : colliding) {
                    if (!entity || entity->IsRemoved()) continue;
                    const double lift = std::max(0.0, top - entity->position.y);
                    if (lift > 0.0) entity->position.y += lift;
                }
            }
            // MC level.setBlock(pos, newState, 2) — clients only.
            if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, withEye, World::UpdateFlags::UpdateClients)) {
                return UseResult::Fail;
            }
            // MC level.updateNeighbourForOutputSignal(pos, END_PORTAL_FRAME):
            // comparators read HAS_EYE (15 with an eye).
            ctx.world->UpdateNeighbourForOutputSignal(pos, BlockID::EndPortalFrame);
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            // MC: level.levelEvent(1503, pos, 0) — the eye-seated sound.
            PlayLevelEventSound(*ctx.world, nullptr, LevelEvent::END_PORTAL_FRAME_FILL, pos, 0,
                                ctx.world->Random());

            const auto match = EndPortalFrame::PortalShapePattern().Find(*ctx.world, pos);
            if (!match) return UseResult::Success;

            // EnderEyeItem.java:52 — the interior's minimum corner, in RAW
            // world offsets. That works because the pattern search is
            // deterministic: the only orientation an inward-facing ring can
            // match is forwards=DOWN, up=SOUTH, which puts frontTopLeft at the
            // ring's maximum X and Z. See EndPortalFrame.hpp.
            const glm::ivec3 base = match->GetFrontTopLeft() + glm::ivec3(-3, 0, -3);
            for (int x = 0; x < 3; ++x) {
                for (int z = 0; z < 3; ++z) {
                    const glm::ivec3 cell{ base.x + x, base.y, base.z + z };
                    // MC level.destroyBlock(cell, true, null) — the pattern's
                    // interior predicate is ANY, so whatever is there drops —
                    // then setBlock(cell, END_PORTAL, 2).
                    ctx.world->DestroyBlock(cell, true);
                    ctx.world->SetBlock(cell.x, cell.y, cell.z, BlockID::EndPortal,
                                        World::UpdateFlags::UpdateClients);
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

            // MC BlockTransformer.transformBlock: gameEvent(BLOCK_CHANGE, pos,
            // Context.of(player, updatedShape)).
            GameEventEmitState(ctx.world, ctx.player, GameEventId::BlockChange, pos);

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
                } else if (JavaRandom* random = ctx.world->Random()) {
                    // CampfireBlock.dowse on the predicting client: 20 ×
                    // makeParticles(signal, smoking) — the doused smoke.
                    const bool signal = cur.GetValueByName("signal_fire") == "true";
                    const ParticleOptions smoke(signal ? ParticleKind::CampfireSignalSmoke
                                                       : ParticleKind::CampfireCosySmoke);
                    JavaRandom& r = *random;
                    for (int j = 0; j < 20; ++j) {
                        const double sx = pos.x + 0.5 + r.NextDouble() / 3.0 * (r.NextBool() ? 1 : -1);
                        const double sy = pos.y + r.NextDouble() + r.NextDouble();
                        const double sz = pos.z + 0.5 + r.NextDouble() / 3.0 * (r.NextBool() ? 1 : -1);
                        ctx.world->DoAddParticle(smoke, true, true, sx, sy, sz, 0.0, 0.07, 0.0);
                        const double px = pos.x + 0.5 + r.NextDouble() / 4.0 * (r.NextBool() ? 1 : -1);
                        const double pz = pos.z + 0.5 + r.NextDouble() / 4.0 * (r.NextBool() ? 1 : -1);
                        ctx.world->AddParticle(ParticleOptions(ParticleKind::Smoke), px, pos.y + 0.4, pz, 0.0, 0.005,
                                               0.0);
                    }
                }
                BlockRegistry::BlockStateDefinition::PropertyMap props;
                props["facing"] = std::string(cur.GetValueByName("facing"));
                props["lit"]    = "false";
                if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, src,
                                         World::UpdateFlags::All, def.IndexOf(props))) {
                    return UseResult::Fail;
                }
                GameEventEmitState(ctx.world, ctx.player, GameEventId::BlockChange, pos);
                UseOnHurtAndBreak(stack, 1, ctx);
                return UseResult::Success;
                // CampfireBlock.dowse is the particles above + a game
                // event only — the food stays on the fire and simply
                // stops cooking, which falls out of the state change above
                // because the block state is what picks the cooking ticker.
            } else {
                return UseResult::Pass;
            }

            // MC: `if (!level.isClientSide()) { setBlock; gameEvent; hurtAndBreak; }`
            const bool ok = ctx.world->SetBlock(pos.x, pos.y, pos.z, newBlock,
                                                World::UpdateFlags::All);
            if (!ok) return UseResult::Fail;
            // BlockTransformer: gameEvent(BLOCK_CHANGE, pos, Context.of(player, updatedShape)).
            GameEventEmitState(ctx.world, ctx.player, GameEventId::BlockChange, pos);
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
                {{BlockID::CopperChest,     BlockID::ExposedCopperChest,      BlockID::WeatheredCopperChest,      BlockID::OxidizedCopperChest},
                 {BlockID::WaxedCopperChest, BlockID::WaxedExposedCopperChest, BlockID::WaxedWeatheredCopperChest, BlockID::WaxedOxidizedCopperChest}},
                // WeatheringCopperGolemStatueBlock / the waxed statues.
                {{BlockID::CopperGolemStatue, BlockID::ExposedCopperGolemStatue, BlockID::WeatheredCopperGolemStatue, BlockID::OxidizedCopperGolemStatue},
                 {BlockID::WaxedCopperGolemStatue, BlockID::WaxedExposedCopperGolemStatue, BlockID::WaxedWeatheredCopperGolemStatue, BlockID::WaxedOxidizedCopperGolemStatue}},
            };
            count = sizeof(families) / sizeof(families[0]);
            return families;
        }

        // MC Block.withPropertiesOf(state): `block`'s default state with every
        // property the two blocks share copied across — how stripping,
        // scraping, waxing and un-waxing keep a stair's facing, a log's axis,
        // a door's half or a copper chest's pairing.
        BlockState WithPropertiesOf(BlockID block, BlockState from) {
            const auto& src = BlockRegistry::GetStateDefinition(from.Block());
            const auto& dst = BlockRegistry::GetStateDefinition(block);
            return BlockStates::FromIndex(block, dst.IndexOf(src.PropertiesOf(from.Index())));
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
                    ctx.world->PlayLevelEvent(ctx.player, LevelEvent::PARTICLES_SCRAPE, pos, 0);   // :79
                } else {
                    newBlock = WaxOffVariant(src);
                    if (newBlock != BlockID::Air) {
                        ctx.world->PlaySound(ctx.player, pos, SoundEvents::AXE_WAX_OFF, SoundSource::Blocks,
                                             1.0f, 1.0f);                       // :83
                        ctx.world->PlayLevelEvent(ctx.player, LevelEvent::PARTICLES_WAX_OFF, pos, 0);  // :84
                    } else {
                        return UseResult::Pass;                         // :86
                    }
                }
            }

            // :54 setBlock(flags 11) / :55 gameEvent / :57 hurtAndBreak. The
            // new state keeps the old one's properties (getStripped /
            // WeatheringCopper.getPrevious / WAX_OFF_BY_BLOCK all hand back
            // `block.withPropertiesOf(state)`).
            const BlockState before = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
            if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, WithPropertiesOf(newBlock, before),
                                     World::UpdateFlags::All)) {
                return UseResult::Fail;
            }
            // BlockTransformer: gameEvent(BLOCK_CHANGE, pos, Context.of(player, updatedShape)).
            GameEventEmitState(ctx.world, ctx.player, GameEventId::BlockChange, pos);
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
            // DyeItem.interactLivingEntity: the stack's DYE (a dye whose
            // component was changed dyes that colour; one without passes).
            const int dye = DyeColorOf(stack);
            if (dye < 0) return UseResult::Pass;
            return InteractEntity_Dye(stack, target, static_cast<uint8_t>(dye));
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
            if (dynamic_cast<const BlockAttachedEntity*>(&target)) return UseResult::Pass;
            // The same for the other plain Entities this port runs as mobs:
            // primed TNT, a falling block, an End crystal and every
            // projectile never reach interactLivingEntity in MC.
            if (dynamic_cast<const PrimedTnt*>(&target) ||
                dynamic_cast<const FallingBlockEntity*>(&target) ||
                dynamic_cast<const EndCrystal*>(&target) ||
                dynamic_cast<const Projectile*>(&target) ||
                IsVehicleEntityType(target.GetType())) {   // boats and minecarts
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

            // SpawnEggItem.getType(stack): the ENTITY_DATA type (every egg's
            // default; a /give'n `entity_data={id:…}` swaps it).
            EntityTypeId type = SpawnEggType(stack);
            if (type == EntityTypeId::Count) type = SpawnEggEntityType(stack.itemId);
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
                // SpawnEggItem.useOn:76 — gameEvent(player, BLOCK_CHANGE, pos).
                GameEventEmit(ctx.world, ctx.player, GameEventId::BlockChange, clicked);
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
                return UseResult::Success;
            }
            // The same for a trial spawner (MC TrialSpawnerBlockEntity is a
            // Spawner too): setEntityId overrides both configs with the
            // egg's type, resets the spawner and turns it INACTIVE.
            if (auto* trial = dynamic_cast<TrialSpawnerBlockEntity*>(ctx.world->GetBlockEntity(clicked))) {
                if (!Rules::GetBool(Rules::Id::SpawnerBlocksWork)) {
                    if (ctx.player) {
                        ctx.player->DisplayClientMessage("Spawner blocks are disabled", /*actionBar=*/false);
                    }
                    return UseResult::Fail;
                }
                trial->SetEntityId(type, *ctx.world);
                ctx.world->BlockEntityChanged(clicked);
                GameEventEmit(ctx.world, ctx.player, GameEventId::BlockChange, clicked);
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
            // EntityType.createDefaultStackConfig: applyComponentsFromItemStack
            // (a renamed egg names its mob; the variant / collar / colour
            // components set them) and the egg's ENTITY_DATA merged over the
            // new mob (updateCustomEntityTag).
            const ItemStack egg = stack;
            const auto applyStackComponents = [&egg](Mob& mob) {
                ApplyDefaultStackConfig(mob, egg, /*userIsPlayer=*/true);
            };
            if (SpawnMobFromItem(type, spawnPos, /*tryMoveDown=*/true, movedUp,
                                 ctx.world->GetDimension(), /*portalCooldownTicks=*/0,
                                 applyStackComponents)) {
                // :101 itemStack.consume(1, user) — only on a successful spawn,
                // so an egg rejected by difficulty is not eaten. Creative is
                // restored by the dispatch's stack snapshot.
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
                // SpawnEggItem.spawnMob:99 — gameEvent(user, ENTITY_PLACE, spawnPos).
                GameEventEmit(ctx.world, ctx.player, GameEventId::EntityPlace, spawnPos);
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
            // :58 setBlock(11); :60 levelEvent 3003 (the wax-on particles).
            // HoneycombItem: waxed.withPropertiesOf(state).
            const BlockState before = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
            if (!ctx.world->SetBlock(pos.x, pos.y, pos.z, WithPropertiesOf(waxed, before),
                                     World::UpdateFlags::All)) {
                return UseResult::Fail;
            }
            ctx.world->PlayLevelEvent(ctx.player, LevelEvent::PARTICLES_WAX_ON, pos, 0);
            // HoneycombItem.useOn:71 — playSound(player, pos, HONEYCOMB_WAX_ON,
            // BLOCKS, 1.0, 1.0).
            ctx.world->PlaySound(ctx.player, pos, SoundEvents::HONEYCOMB_WAX_ON, SoundSource::Blocks, 1.0f, 1.0f);
            // HoneycombItem.useOn:69 — gameEvent(BLOCK_CHANGE, pos, Context.of(player, waxedState)).
            GameEventEmitState(ctx.world, ctx.player, GameEventId::BlockChange, pos);
            return UseResult::Success;
        }

        // ── Bone meal — mirrors BoneMealItem.java:35-61 ─────────────────────
        // growCrop (any BonemealableBlock — crops, stems, bamboo, cocoa, berry
        // bushes, the grass block, short grass, mushrooms, sea pickles) is
        // here. The water/seagrass branch (growWaterPlant) is still skipped;
        // it needs coral biome tags.
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
            // isBonemealSuccess is vanilla's default `true` unless the block
            // declares the hook (a mushroom's 40 % roll); the item is spent
            // whichever way the roll goes.
            {
                const Block& def = BlockRegistry::Get(src);
                if (def.performBonemeal && def.isValidBonemealTarget) {
                    const BlockState state = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
                    if (def.isValidBonemealTarget(*ctx.world, pos, state)) {
                        // growCrop: `if (level instanceof ServerLevel)` — the
                        // growth, its roll and the shrink are the SERVER's.
                        // The client answers SUCCESS and changes nothing: it
                        // would roll its own random, draw a different crop
                        // stage or grass pattern, and the server's blocks
                        // would visibly replace it a moment later. The
                        // plants, the particles (levelEvent 1505) and the
                        // spent bone meal all arrive from the server.
                        if (ctx.world->IsClientSide()) return UseResult::Success;
                        JavaRandom fallback(static_cast<int64_t>(pos.x) * 341873128712LL +
                                            static_cast<int64_t>(pos.z) * 132897987541LL +
                                            static_cast<int64_t>(pos.y));
                        JavaRandom* levelRandom = ctx.world->Random();
                        JavaRandom& random = levelRandom ? *levelRandom : fallback;   // level.getRandom()
                        if (!def.isBonemealSuccess ||
                            def.isBonemealSuccess(*ctx.world, pos, state, random)) {
                            def.performBonemeal(*ctx.world, pos, state, random);
                        }

                        // MC: `level.levelEvent(1505, pos, 15)` →
                        // BoneMealItem.addGrowthParticles spawns 15
                        // happy_villager particles inside the block. Sent
                        // from the server (`if (!level.isClientSide())`) to
                        // everyone: PlayLevelEventSound carries the particle
                        // half as well (Client::LevelEvents on the receiver).
                        if (!ctx.world->IsClientSide()) {
                            // BoneMealItem.useOn:43 — boneMealStack.causeUseVibration(
                            // player, ITEM_INTERACT_FINISH) (UseEffects.DEFAULT: on).
                            if (Entity* user = ctx.player ? ctx.player->GameEventSource() : nullptr) {
                                user->GameEvent(GameEventId::ItemInteractFinish);
                            }
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

            // The grass block is an ordinary BonemealableBlock now
            // (GrassBlock.performBonemeal, PlantBlocks.cpp), so it went
            // through growCrop above — server-side, with the level's random,
            // and with the level event the client's growth particles read.
            // growWaterPlant (seagrass / coral from bone meal in water) is not
            // ported yet: PASS, as vanilla returns when neither branch takes.
            return UseResult::Pass;
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
                                            bool stopOnFluid,
                                            float kReach = 5.0f) {   // blockInteractionRange
            // Eye + direction from the server-authoritative rotation (the
            // UseItemC2S handler snapped it to the click's exact aim).
            const glm::dvec3 p = player.getPosition();
            const glm::vec3 eye(static_cast<float>(p.x),
                                static_cast<float>(p.y) + 1.62f,
                                static_cast<float>(p.z));
            const glm::vec3 dir =
                Mth::ViewVector(player.getPitch(), player.getYaw());


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

            const ItemStack bucket = stack;
            const auto configure = [bucket](Mob& mob) {
                if (auto* cube = dynamic_cast<SulfurCube*>(&mob)) cube->LoadFromBucket(bucket);
            };
            if (!SpawnMobFromItem(EntityTypeId::SulfurCube, hit->beforePos,
                                  /*tryMoveDown=*/false, /*movedUp=*/false,
                                  world->GetDimension(), 0, configure)) {
                return UseResult::Fail;
            }
            // MobBucketItem.spawn:36 — gameEvent(user, ENTITY_PLACE, pos).
            GameEventEmit(world, player, GameEventId::EntityPlace, hit->beforePos);
            if (!player->isCreative()) {
                stack = ItemStack(Items::Bucket, 1);
                player->markSlotDirty(player->handSlotIndex(hand));
            }
            return UseResult::Success;
        }

        // Bucket of cod / salmon / pufferfish / tropical fish / axolotl /
        // tadpole — MC MobBucketItem(type, WATER, emptySound): BucketItem.use
        // pours the water (with the mob's empty sound, NEUTRAL), then
        // checkExtraContent → spawn: EntityType.create(…, BUCKET, tryMoveDown
        // true, movedUp false) at the cell the water went to, the stack's
        // CUSTOM_NAME and implicit components (AXOLOTL_VARIANT, SALMON_SIZE,
        // TROPICAL_FISH_*) applied,
        // loadFromBucketTag(BUCKET_ENTITY_DATA) + setFromBucket(true), the
        // mob added and its ambient sound played; then the empty bucket.
        bool EmptyBucketContents(ILevelWrite* world, IUsePlayer* player, const BucketHit& hit,
                                 bool isLava, const char* emptySound, glm::ivec3& outTarget);   // below

        UseResult Use_MobBucket(ILevelWrite* world, IUsePlayer* player,
                                uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            const auto mobBucket = Bucketable::MobBucketFor(stack.itemId);
            if (!mobBucket) return UseResult::Pass;
            // MobBucketItem.getFluidContext: ClipContext.Fluid.NONE.
            auto hit = BucketClip(world, *player, /*stopOnFluid=*/false);
            if (!hit) return UseResult::Pass;
            glm::ivec3 target;
            if (!EmptyBucketContents(world, player, *hit, /*isLava=*/false, mobBucket->emptySound, target)) {
                return UseResult::Fail;
            }
            if (!world->IsClientSide()) {
                const ItemStack bucket = stack;
                const auto configure = [bucket](Mob& mob) {
                    // EntityType.createDefaultStackConfig: the custom name
                    // and the type's implicit components.
                    // MobBucketItem.spawn: EntityType.create(…, createDefault-
                    // StackConfig(level, bucket, user)) — the custom name and
                    // the mob's implicit components (variants) — then
                    // loadFromBucketTag.
                    ApplyComponentsFromItemStack(mob, bucket);
                    const BucketEntityData data =
                        bucket.components.get(DataComponents::BUCKET_ENTITY_DATA).value_or(BucketEntityData{});
                    if (auto* fish = dynamic_cast<Fish*>(&mob)) {
                        // SALMON_SIZE / TROPICAL_FISH_* (applyImplicitComponents).
                        fish->ApplyImplicitComponents(bucket);
                        fish->LoadFromBucket(data);
                        fish->SetFromBucket(true);
                    } else if (auto* axolotl = dynamic_cast<Axolotl*>(&mob)) {
                        if (auto v = bucket.components.get(DataComponents::AXOLOTL_VARIANT);
                            v && *v >= 0 && *v < Axolotl::kVariantCount) {
                            axolotl->SetVariant(static_cast<Axolotl::Variant>(*v));
                        }
                        axolotl->LoadFromBucket(data);
                        axolotl->SetFromBucket(true);
                    } else if (auto* tadpole = dynamic_cast<Tadpole*>(&mob)) {
                        tadpole->LoadFromBucket(data);   // fromBucket is always true
                    }
                    mob.PlayAmbientSound();
                };
                if (SpawnMobFromItem(mobBucket->type, target, /*tryMoveDown=*/true, /*movedUp=*/false,
                                     world->GetDimension(), 0, configure, SpawnReason::Bucket)) {
                    // MobBucketItem.spawn:36 — gameEvent(user, ENTITY_PLACE, pos).
                    GameEventEmit(world, player, GameEventId::EntityPlace, target);
                }
            }
            // BucketItem.getEmptySuccessItem — creative keeps the full one.
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
            if (!ThrowEnderEye(player->getDimensionId(), from, stack, player)) {
                // MC returns CONSUME with the stack UNTOUCHED when there is no
                // structure to point at — the eye is not spent on a world that
                // has nowhere to send it.
                return UseResult::Consume;
            }

            // EnderEyeItem.use:103 — at the player, NEUTRAL, pitch
            // lerp(nextFloat, 0.33, 0.5); server-side, for everyone.
            // EnderEyeItem.use:95 — gameEvent(PROJECTILE_SHOOT, eye position,
            // Context.of(player)).
            world->GameEvent(GameEventId::ProjectileShoot, from,
                             GameEventContext::Of(player->GameEventSource()));
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

        // ── FishingRod.use — mirrors FishingRodItem.java ───────────────────
        //
        // Cast when the player has no hook out, reel in when they do; both
        // halves are the server's (the hook, the loot, the orbs and the rod's
        // wear live there — FishingRodUse.cpp). MC's client runs the same
        // use and gets nothing from it but SUCCESS: its level.playSound(null,
        // ...) plays nothing client-side, and the hook arrives on the wire.
        // So the client prediction is exactly that — the arm swing.
        UseResult Use_FishingRod(ILevelWrite* world, IUsePlayer* player,
                                 uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;
            return UseFishingRodServer(world, *player, hand, stack);
        }

        // ── WindCharge.use — mirrors WindChargeItem.java ───────────────────
        //
        // Throw a WindCharge from the player's eye (power 1.5, inaccuracy
        // 1.0), WIND_CHARGE_THROW for everyone, spend one. The 0.5 s
        // use_cooldown is applied by the dispatcher (ItemStack.use's
        // applyAfterUseComponentSideEffects → UseCooldown, ItemCooldowns).
        UseResult Use_WindCharge(ILevelWrite* world, IUsePlayer* player,
                                 uint32_t /*hand*/, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;

            // `if (level instanceof ServerLevel)` guards the spawn; the
            // client still answers SUCCESS and waits for the entity (and the
            // shrunk stack) from the server, as for the pearl.
            if (world->IsClientSide()) return UseResult::Success;

            if (!ThrowWindCharge(player->getDimensionId(), *player)) {
                return UseResult::Fail;
            }

            // WindChargeItem.use:38 — playSound(null, player x/y/z,
            // WIND_CHARGE_THROW, NEUTRAL, 0.5, 0.4 / (nextFloat * 0.4 + 0.8)).
            world->PlaySound(nullptr, player->getPosition(), SoundEvents::WIND_CHARGE_THROW, SoundSource::Neutral,
                             0.5f, 0.4f / (LevelRandomFloat(world) * 0.4f + 0.8f));
            // stack.consume(1, player) — creative keeps the charge.
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
                // BucketItem.use:84 — gameEvent(player, FLUID_PICKUP, pos).
                GameEventEmit(world, player, GameEventId::FluidPickup, hit->pos);
                {
                    // BucketItem.use:83 — CriteriaTriggers.FILLED_BUCKET.
                    if (!world->IsClientSide()) {
                        if (Server::ServerPlayer* sp = Server::CriteriaTriggers::PlayerOf(player)) {
                            Server::CriteriaTriggers::FilledBucket(*sp, ItemStack(filledWater, 1));
                        }
                    }
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
            // BucketItem.use:84 — gameEvent(player, FLUID_PICKUP, pos).
            GameEventEmit(world, player, GameEventId::FluidPickup, hit->pos);
            // ItemUtils.createFilledResult (:62): creative keeps the empty
            // bucket, survival transforms it. Component patch reset — a
            // fresh filled bucket carries no per-stack state.
            // ItemUtils.createFilledResult: one bucket of the stack fills,
            // the rest stays (creative keeps the stack and gains the filled
            // bucket once).
            const ItemStack filledBucket(hit->block == BlockID::Lava ? Items::LavaBucket : filledWater, 1);
            // BucketItem.use:83 — CriteriaTriggers.FILLED_BUCKET.
            if (!world->IsClientSide()) {
                if (Server::ServerPlayer* sp = Server::CriteriaTriggers::PlayerOf(player)) {
                    Server::CriteriaTriggers::FilledBucket(*sp, filledBucket);
                }
            }
            player->CreateFilledResult(stack, filledBucket);
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
            // BottleItem.use:60 — gameEvent(player, FLUID_PICKUP, pos).
            GameEventEmit(world, player, GameEventId::FluidPickup, hit->pos);
            player->CreateFilledResult(stack, CreatePotionItemStack(Items::Potion, PotionId::Water));
            player->markSlotDirty(player->handSlotIndex(hand));
            return UseResult::Success;
        }

        // BucketItem.playEmptySound (BLOCKS) — or, for a MobBucketItem, its
        // own emptySound on NEUTRAL (MobBucketItem.playEmptySound).
        void PlayBucketEmptySound(ILevelWrite* world, IUsePlayer* player, const glm::ivec3& pos,
                                  bool isLava, const char* emptySound) {
            if (emptySound) {
                world->PlaySound(player, pos, emptySound, SoundSource::Neutral, 1.0f, 1.0f);
            } else {
                world->PlaySound(player, pos, isLava ? SoundEvents::BUCKET_EMPTY_LAVA : SoundEvents::BUCKET_EMPTY,
                                 SoundSource::Blocks, 1.0f, 1.0f);
                // BucketItem.playEmptySound:191 — gameEvent(user, FLUID_PLACE,
                // pos). (MobBucketItem's override plays its sound only.)
                GameEventEmit(world, player, GameEventId::FluidPlace, pos);
            }
        }

        // MC BucketItem.emptyContents (:104-179) for a water or lava bucket
        // aimed at `hit` (simplified: no fluid simulation, sources are
        // static cubes). True when the contents went somewhere — poured,
        // waterlogged into the clicked block, or evaporated in the nether —
        // with `outTarget` the cell they went to (MobBucketItem spawns its
        // mob there). `emptySound` null = the plain bucket's sound.
        bool EmptyBucketContents(ILevelWrite* world, IUsePlayer* player, const BucketHit& hit,
                                 bool isLava, const char* emptySound, glm::ivec3& outTarget) {
            // MC BucketItem.java:83-84:
            //   BlockPos target = state.getBlock() instanceof LiquidBlockContainer
            //                     && this.content == Fluids.WATER ? pos : relativePos;
            //
            // Pouring WATER onto a waterloggable block fills the block itself
            // rather than the cell in front of it — that is how you waterlog a
            // fence or a stair. Lava is excluded by the `content == WATER`
            // clause, exactly as vanilla has it, so a lava bucket still pours
            // into the adjacent cell.
            if (!isLava && BlockRegistry::IsWaterloggable(hit.block)) {
                const BlockState hitState =
                    world->GetBlockState(hit.pos.x, hit.pos.y, hit.pos.z);
                // SimpleWaterloggedBlock.placeLiquid:24 refuses when the block
                // is already waterlogged, and the refusal propagates all the
                // way out as a failed use — the bucket is not consumed.
                if (BlockRegistry::ContainsWater(hitState)) {
                    return false;
                }
                world->SetBlock(hit.pos.x, hit.pos.y, hit.pos.z,
                                BlockRegistry::WithWaterlogged(hitState, true),
                                World::UpdateFlags::All);
                // SimpleWaterloggedBlock.placeLiquid's second half: the new
                // water books its first tick so it can start flowing out of
                // the block. A no-op on the client (no scheduler).
                Fluids::ScheduleTick(*world, hit.pos, FluidType::Water);
                // BucketItem.emptyContents → playEmptySound(user, level, pos).
                PlayBucketEmptySound(world, player, hit.pos, isLava, emptySound);
                outTarget = hit.pos;
                return true;
            }

            // BucketItem.use:84 — the cell in front of the clicked face
            // (pos.relative(direction)); the clicked cell itself only for
            // water into a LiquidBlockContainer, handled above.
            glm::ivec3 target = hit.beforePos;
            if (!world->IsValidPosition(target.x, target.y, target.z)) {
                return false;
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
                return false;                                     // :88
            }

            // EnvironmentAttributes.WATER_EVAPORATES (the nether's ultrawarm
            // flag): water poured in the nether hisses away (with eight
            // LARGE_SMOKE puffs) and the bucket still empties.
            if (!isLava && world->GetDimension() == DimensionId::Nether) {
                float pitch = 2.6f;
                if (JavaRandom* random = world->Random()) {
                    pitch += (random->NextFloat() - random->NextFloat()) * 0.8f;
                }
                // BucketItem.emptyContents:154 — playSound(user, pos, FIRE_EXTINGUISH, …).
                world->PlaySound(player, target, SoundEvents::FIRE_EXTINGUISH, SoundSource::Blocks, 0.5f, pitch);
                // :157 — sendParticles(LARGE_SMOKE, x, y, z, 8, 1, 1, 1, 0,
                // RandomizationType.ALTERNATIVE): the steam over the cell.
                if (!world->IsClientSide()) {
                    if (Particles::ServerParticleSink* sink = Particles::GetServerSink()) {
                        Particles::ParticleBurst burst;
                        burst.options = ParticleOptions(ParticleKind::LargeSmoke);
                        burst.pos = glm::dvec3(target);
                        burst.dist = glm::vec3(1.0f);
                        burst.count = 8;
                        burst.randomization = Particles::Randomization::Alternative;
                        sink->SendParticles(world->GetDimension(), burst);
                    }
                }
                outTarget = target;
                return true;
            }

            // `if (!isClientSide && mayReplace && !blockState.liquid())
            //     level.destroyBlock(pos, true)` — the tall grass the water
            // displaces drops as an item.
            const bool targetIsLiquid = targetBlock == BlockID::Water || targetBlock == BlockID::Lava;
            if (!world->IsClientSide() && mayReplace && !targetIsLiquid &&
                targetBlock != BlockID::Air) {
                // Level.destroyBlock's levelEvent 2001 (the plant's puff).
                PlayLevelEventSound(*world, nullptr, LevelEvent::PARTICLES_DESTROY_BLOCK, target,
                                    static_cast<int>(world->GetBlockState(target.x, target.y, target.z).RawId()),
                                    world->Random());
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
                return false;
            }
            // BucketItem.playEmptySound:188 — (user, pos, BUCKET_EMPTY(_LAVA), BLOCKS).
            PlayBucketEmptySound(world, player, target, isLava, emptySound);   // :175-179
            outTarget = target;
            return true;                                      // :90
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

            // MC BucketItem.use:85 — emptyContents, then the success item.
            glm::ivec3 target;
            if (!EmptyBucketContents(world, player, *hit, isLava, nullptr, target)) {
                return UseResult::Fail;
            }
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

        // The spears' Item.use (a KINETIC_WEAPON): the charge's hold and the
        // use sound (server: WeaponItems::SpearBegin). The client's own half
        // — the pose, its local sound — is PlayerController's prediction.
        UseResult Use_Spear(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                            ItemStack& /*stack*/) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Consume;
            return WeaponItems::SpearBegin(*player, hand);
        }

        // TridentItem.use: the draw, unless it would break or Riptide has no
        // water or rain (server: WeaponItems::TridentBegin).
        UseResult Use_Trident(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                              ItemStack& /*stack*/) {
            if (!world || !player) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Consume;
            return WeaponItems::TridentBegin(*player, hand);
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

        // ── Goat horn — mirrors InstrumentItem.use ───────────────────────
        // The stack's INSTRUMENT (the goat horn's default is ponder): no
        // instrument, FAIL. Otherwise the hold (use_duration * 20 ticks,
        // TOOT_HORN) and its cooldown are the server's (Archaeology::
        // GoatHornBegin); InstrumentItem.play runs on both sides here as in
        // MC — level.playSound(player, player, sound, RECORDS, range / 16, 1)
        // is this client's own copy while predicting and everyone else's
        // from the server.
        UseResult Use_GoatHorn(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                               ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            const std::optional<std::string> id = stack.get(DataComponents::INSTRUMENT);
            const Instruments::Instrument* instrument = id ? Instruments::Get(*id) : nullptr;
            if (!instrument) return UseResult::Fail;
            if (world->IsClientSide()) {
                world->PlaySound(SoundExcept(player), player->getPosition(), instrument->soundEvent,
                                 SoundSource::Records, instrument->range / 16.0f, 1.0f);
                return UseResult::Consume;
            }
            const UseResult begun = Archaeology::GoatHornBegin(*player, hand, stack);
            if (!ConsumesAction(begun)) return begun;
            world->PlaySound(SoundExcept(player), player->getPosition(), instrument->soundEvent,
                             SoundSource::Records, instrument->range / 16.0f, 1.0f);
            // level.gameEvent(GameEvent.INSTRUMENT_PLAY, player.position(),
            // GameEvent.Context.of(player)).
            world->GameEvent(GameEventId::InstrumentPlay, player->getPosition(),
                             GameEventContext::Of(player->GameEventSource()));
            return begun;
        }

    } // namespace

    // ── Archaeology: the brush's view ray ────────────────────────────────
    // BrushItem.calculateHitResult (ProjectileUtil.getHitResultOnViewVector
    // over blockInteractionRange) reduced to its block half: the bucket's
    // POV clip, fluids ignored, and the face the ray entered through.
    std::optional<Archaeology::ViewBlockHit> Archaeology::ClipPlayerView(ILevelWrite& world,
                                                                         const IUsePlayer& player,
                                                                         float reach) {
        auto hit = BucketClip(&world, player, /*stopOnFluid=*/false, reach);
        if (!hit) return std::nullopt;
        ViewBlockHit out;
        out.pos = hit->pos;
        const glm::ivec3 d = hit->beforePos - hit->pos;
        // Direction 3D data values: down, up, north, south, west, east. A ray
        // that starts inside its block has no entry face; UP stands in.
        if      (d.y < 0) out.face = 0;
        else if (d.y > 0) out.face = 1;
        else if (d.z < 0) out.face = 2;
        else if (d.z > 0) out.face = 3;
        else if (d.x < 0) out.face = 4;
        else if (d.x > 0) out.face = 5;
        else              out.face = 1;
        // BlockHitResult.getLocation: the ray's crossing of the struck face's
        // plane (the cell's side; the ray starting inside the block keeps the
        // eye).
        const glm::dvec3 p = player.getPosition();
        const glm::dvec3 eye(p.x, p.y + 1.62, p.z);
        const glm::dvec3 dir = glm::dvec3(Mth::ViewVector(player.getPitch(), player.getYaw()));
        out.viewVector = dir;
        out.location = eye;
        const int axis = (out.face <= 1) ? 1 : (out.face <= 3 ? 2 : 0);
        const bool positive = out.face == 1 || out.face == 3 || out.face == 5;
        const double plane = static_cast<double>(out.pos[axis]) + (positive ? 1.0 : 0.0);
        if (std::abs(dir[axis]) > 1e-9 && d != glm::ivec3(0)) {
            const double t = (plane - eye[axis]) / dir[axis];
            if (t >= 0.0) out.location = eye + dir * t;
        }
        return out;
    }

    // WeatheringCopper.getAge for the ChangeOverTimeBlock family — declared in
    // CopperChestBlock.hpp; answered from the family table above so the
    // weathering neighbour count and the axe/honeycomb maps cannot disagree.
    int WeatheringCopperAge(BlockID id) {
        size_t count = 0;
        const CopperFamily* families = CopperFamilies(count);
        for (size_t f = 0; f < count; ++f) {
            for (int s = 0; s < 4; ++s) {
                if (families[f].stage[s] == id) return s;
            }
        }
        return -1;
    }

    BlockState WeatheringCopperPreviousState(BlockState state) {
        // WeatheringCopper.getPrevious(state): one stage back, properties
        // kept; the state itself when there is none (unaffected / not copper).
        const BlockID previous = CopperScrapedVariant(state.Block());
        return previous == BlockID::Air ? state : WithPropertiesOf(previous, state);
    }

    BlockState WeatheringCopperFirstState(BlockState state) {
        // WeatheringCopper.getFirst(state): the family's unaffected stage.
        size_t count = 0;
        const CopperFamily* families = CopperFamilies(count);
        for (size_t f = 0; f < count; ++f) {
            for (int s = 0; s < 4; ++s) {
                if (families[f].stage[s] == state.Block()) {
                    return s == 0 ? state : WithPropertiesOf(families[f].stage[0], state);
                }
            }
        }
        return state;
    }

    bool IsWaxedCopperBlock(BlockID id) {
        // HoneycombItem.WAX_OFF_BY_BLOCK.containsKey — a waxed stage.
        size_t count = 0;
        const CopperFamily* families = CopperFamilies(count);
        for (size_t f = 0; f < count; ++f) {
            for (int s = 0; s < 4; ++s) {
                if (families[f].waxed[s] == id) return true;
            }
        }
        return false;
    }

    BlockID WeatheringCopperNext(BlockID id) {
        size_t count = 0;
        const CopperFamily* families = CopperFamilies(count);
        for (size_t f = 0; f < count; ++f) {
            for (int s = 0; s < 3; ++s) {
                if (families[f].stage[s] == id) return families[f].stage[s + 1];
            }
        }
        return BlockID::Air;
    }

    ItemUseOnFn BlockTransformerUseOn(std::string_view key) {
        // The BlockTransformers registry (AXE / HOE / SHOVEL): the engine's
        // strip-scrape-wax, till and flatten behaviours.
        if (key.rfind("minecraft:", 0) == 0) key.remove_prefix(10);
        if (key == "axe")    return &UseOn_Axe;
        if (key == "hoe")    return &UseOn_Hoe;
        if (key == "shovel") return &UseOn_Shovel;
        return nullptr;
    }

    void ItemRegistry_RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems) {
        auto wireUseOn = [&](ItemID id, ItemUseOnFn fn) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) it->second.useOn = fn;
        };
        // Item.Properties.axe / hoe / shovel: BLOCK_TRANSFORMER, the holder
        // Item.useOn applies (GameplayDataComponents' StackUseOn).
        auto setTransformer = [&](ItemID id, const char* key) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) {
                it->second.defaultComponents.set(DataComponents::BLOCK_TRANSFORMER, BlockTransformerHolder{key, {}});
            }
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
        // DEBUG_STICK: DebugStickItem.useOn, and Items.java's
        // .component(DEBUG_STICK_STATE, DebugStickState.EMPTY).
        wireUseOn(Items::DebugStick, &BlockData::UseOnDebugStick);
        if (auto it = pureItems.find(Items::DebugStick); it != pureItems.end()) {
            it->second.defaultComponents.set(DataComponents::DEBUG_STICK_STATE, DebugStickState{});
        }
        // FireChargeItem.useOn — lights campfires, candles and candle cakes,
        // else sets a fire; used up either way it succeeds.
        wireUseOn(Items::FireCharge, &UseOn_FireCharge);

        // EnderEye — seats an eye in an end_portal_frame and opens the portal
        // once the ring is complete. Wiring a useOn also takes the eye out of
        // the client's block-placement prediction (PlayerController refuses to
        // predict placement for any item that has one), which is what stops it
        // predicting a block that is not a block item at all.
        wireUseOn(Items::EnderEye, &UseOn_EnderEye);

        // Brush — MC BrushItem: useOn starts the 200-tick BRUSH hold (no air
        // use: Item.use's default passes), and every tick of it is
        // onUseTick's stroke check (Archaeology::BrushUseTick).
        wireUseOn(Items::Brush, &Archaeology::BrushUseOn);
        if (auto it = pureItems.find(Items::Brush); it != pureItems.end()) {
            it->second.useDuration  = Archaeology::kBrushUseDuration;
            it->second.useAnimation = ItemUseAnimation::BRUSH;
            it->second.onUseTick    = &Archaeology::BrushUseTick;
        }

        // Goat horn — MC InstrumentItem: the default INSTRUMENT
        // (Instruments.PONDER_GOAT_HORN, Items.java), use plays it and holds
        // TOOT_HORN for the instrument's use_duration (every vanilla horn:
        // 7 s = 140 ticks).
        if (auto it = pureItems.find(Items::GoatHorn); it != pureItems.end()) {
            it->second.defaultComponents.set(DataComponents::INSTRUMENT,
                                             std::string(Instruments::kDefaultGoatHorn));
            it->second.use          = &Use_GoatHorn;
            it->second.useAnimation = ItemUseAnimation::TOOT_HORN;
            // InstrumentItem.getUseDuration is floor(use_duration * 20) of
            // the stack's instrument; the engine's use duration is per item,
            // and every vanilla goat horn's use_duration is 7.0. (Not read
            // from the data pack here: item registration can run before the
            // data root is reachable.)
            it->second.useDuration = 140;
        }

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
            // MC TridentItem: 72000 ticks, TRIDENT, thrown (or riptided) on
            // release (WeaponItems).
            setHold(Items::Trident, &Use_Trident, 72000, ItemUseAnimation::TRIDENT,
                    nullptr, &WeaponItems::TridentRelease);
            // The spears' KINETIC_WEAPON (Item.getUseDuration 72000,
            // getUseAnimation SPEAR): the charge, whose every tick is
            // ItemStack.onUseTick → KineticWeapon.damageEntities.
            for (ItemID spear : { Items::WoodenSpear, Items::StoneSpear, Items::CopperSpear,
                                  Items::IronSpear, Items::GoldenSpear, Items::DiamondSpear,
                                  Items::NetheriteSpear }) {
                setHold(spear, &Use_Spear, Spear::kUseDuration, ItemUseAnimation::SPEAR, nullptr, nullptr);
                if (auto it = pureItems.find(spear); it != pureItems.end()) {
                    it->second.onUseTick = &WeaponItems::SpearUseTick;
                }
            }
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
            setTransformer(id, "minecraft:hoe");
        }

        // Same for every shovel tier.
        for (ItemID id : {
                Items::WoodenShovel, Items::CopperShovel, Items::StoneShovel,
                Items::GoldenShovel, Items::IronShovel,   Items::DiamondShovel,
                Items::NetheriteShovel, Items::ResoniteShovel,
                // the Aether's and Twilight Forest's tiers (docs/mod-ports.md)
                Items::SkyrootShovel, Items::HolystoneShovel, Items::ZaniteShovel, Items::GravititeShovel, Items::IronwoodShovel, Items::SteeleafShovel }) {
            wireUseOn(id, &UseOn_Shovel);
            setTransformer(id, "minecraft:shovel");
        }

        // Every axe tier shares strip/scrape/wax-off (AxeItem.java:38-105).
        for (ItemID id : {
                Items::WoodenAxe, Items::CopperAxe, Items::StoneAxe,
                Items::GoldenAxe, Items::IronAxe,   Items::DiamondAxe,
                Items::NetheriteAxe, Items::ResoniteAxe,
                // the Aether's and Twilight Forest's tiers (docs/mod-ports.md)
                Items::SkyrootAxe, Items::HolystoneAxe, Items::ZaniteAxe, Items::GravititeAxe, Items::IronwoodAxe, Items::SteeleafAxe, Items::KnightmetalAxe }) {
            wireUseOn(id, &UseOn_Axe);
            setTransformer(id, "minecraft:axe");
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
        wireUse(Items::WindCharge,  &Use_WindCharge);
        wireUse(Items::FishingRod,  &Use_FishingRod);
        wireUse(Items::Bucket,      &Use_EmptyBucket);
        wireUse(Items::GlassBottle, &Use_GlassBottle);
        wireUse(Items::SulfurCubeBucket, &Use_SulfurCubeBucket);
        for (ItemID id : { Items::PufferfishBucket, Items::SalmonBucket, Items::CodBucket,
                           Items::TropicalFishBucket, Items::AxolotlBucket, Items::TadpoleBucket }) {
            wireUse(id, &Use_MobBucket);
        }
        wireUse(Items::WaterBucket, &Use_FilledBucket);
        wireUse(Items::LavaBucket,  &Use_FilledBucket);
        // The Aether's skyroot buckets (SkyrootBucketItem): the same use, their
        // own empty/filled pair (see Use_EmptyBucket / Use_FilledBucket).
        wireUse(Items::SkyrootBucket,      &Use_EmptyBucket);
        wireUse(Items::SkyrootWaterBucket, &Use_FilledBucket);
        // Maps: EmptyMapItem.use on `map`, MapItem.useOn (banner markers) on
        // filled_map and the 26.3 structure maps.
        MapItem::RegisterBehaviors(pureItems);
        // FoodOnAStickItem.use: carrot / warped fungus on a stick boost the
        // pig / strider they steer.
        FoodOnAStickItem::RegisterBehaviors(pureItems);
        // Filled buckets stack to 1 (Items.java `.stacksTo(1)` on all buckets;
        // the empty bucket stacks to 16).
        if (auto it = pureItems.find(Items::Bucket); it != pureItems.end())
            it->second.maxStackSize = 16;
        for (ItemID id : { Items::WaterBucket, Items::LavaBucket,
                           Items::PufferfishBucket, Items::SalmonBucket, Items::CodBucket,
                           Items::TropicalFishBucket, Items::AxolotlBucket, Items::TadpoleBucket }) {
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
        setTool(Items::WoodenPickaxe,    MakeDiggerTool(ToolType::Pickaxe, MiningTier::Wood, 2.0f));
        setTool(Items::CopperPickaxe,    MakeDiggerTool("#minecraft:incorrect_for_copper_tool", MineableTag(ToolType::Pickaxe), 5.0f));
        setTool(Items::StonePickaxe,     MakeDiggerTool(ToolType::Pickaxe, MiningTier::Stone, 4.0f));
        setTool(Items::GoldenPickaxe,    MakeDiggerTool(ToolType::Pickaxe, MiningTier::Gold, 12.0f));
        setTool(Items::IronPickaxe,      MakeDiggerTool(ToolType::Pickaxe, MiningTier::Iron, 6.0f));
        setTool(Items::DiamondPickaxe,   MakeDiggerTool(ToolType::Pickaxe, MiningTier::Diamond, 8.0f));
        setTool(Items::NetheritePickaxe, MakeDiggerTool(ToolType::Pickaxe, MiningTier::Netherite, 9.0f));
        setTool(Items::ResonitePickaxe,  MakeDiggerTool(ToolType::Pickaxe, MiningTier::Resonite, 9.0f));
        // Axes
        setTool(Items::WoodenAxe,        MakeDiggerTool(ToolType::Axe, MiningTier::Wood, 2.0f));
        setTool(Items::CopperAxe,        MakeDiggerTool("#minecraft:incorrect_for_copper_tool", MineableTag(ToolType::Axe), 5.0f));
        setTool(Items::StoneAxe,         MakeDiggerTool(ToolType::Axe, MiningTier::Stone, 4.0f));
        setTool(Items::GoldenAxe,        MakeDiggerTool(ToolType::Axe, MiningTier::Gold, 12.0f));
        setTool(Items::IronAxe,          MakeDiggerTool(ToolType::Axe, MiningTier::Iron, 6.0f));
        setTool(Items::DiamondAxe,       MakeDiggerTool(ToolType::Axe, MiningTier::Diamond, 8.0f));
        setTool(Items::NetheriteAxe,     MakeDiggerTool(ToolType::Axe, MiningTier::Netherite, 9.0f));
        setTool(Items::ResoniteAxe,      MakeDiggerTool(ToolType::Axe, MiningTier::Resonite, 9.0f));
        // Shovels
        setTool(Items::WoodenShovel,     MakeDiggerTool(ToolType::Shovel, MiningTier::Wood, 2.0f));
        setTool(Items::CopperShovel,     MakeDiggerTool("#minecraft:incorrect_for_copper_tool", MineableTag(ToolType::Shovel), 5.0f));
        setTool(Items::StoneShovel,      MakeDiggerTool(ToolType::Shovel, MiningTier::Stone, 4.0f));
        setTool(Items::GoldenShovel,     MakeDiggerTool(ToolType::Shovel, MiningTier::Gold, 12.0f));
        setTool(Items::IronShovel,       MakeDiggerTool(ToolType::Shovel, MiningTier::Iron, 6.0f));
        setTool(Items::DiamondShovel,    MakeDiggerTool(ToolType::Shovel, MiningTier::Diamond, 8.0f));
        setTool(Items::NetheriteShovel,  MakeDiggerTool(ToolType::Shovel, MiningTier::Netherite, 9.0f));
        setTool(Items::ResoniteShovel,   MakeDiggerTool(ToolType::Shovel, MiningTier::Resonite, 9.0f));
        // Hoes
        setTool(Items::WoodenHoe,        MakeDiggerTool(ToolType::Hoe, MiningTier::Wood, 2.0f));
        setTool(Items::CopperHoe,        MakeDiggerTool("#minecraft:incorrect_for_copper_tool", MineableTag(ToolType::Hoe), 5.0f));
        setTool(Items::StoneHoe,         MakeDiggerTool(ToolType::Hoe, MiningTier::Stone, 4.0f));
        setTool(Items::GoldenHoe,        MakeDiggerTool(ToolType::Hoe, MiningTier::Gold, 12.0f));
        setTool(Items::IronHoe,          MakeDiggerTool(ToolType::Hoe, MiningTier::Iron, 6.0f));
        setTool(Items::DiamondHoe,       MakeDiggerTool(ToolType::Hoe, MiningTier::Diamond, 8.0f));
        setTool(Items::NetheriteHoe,     MakeDiggerTool(ToolType::Hoe, MiningTier::Netherite, 9.0f));
        setTool(Items::ResoniteHoe,      MakeDiggerTool(ToolType::Hoe, MiningTier::Resonite, 9.0f));
        // Swords — ToolMaterial.applySwordProperties: the same TOOL for every
        // material (cobweb 15, #sword_instantly_mines, #sword_efficient 1.5;
        // no creative breaking).
        setTool(Items::WoodenSword,      MakeSwordTool());
        setTool(Items::CopperSword,      MakeSwordTool());
        setTool(Items::StoneSword,       MakeSwordTool());
        setTool(Items::GoldenSword,      MakeSwordTool());
        setTool(Items::IronSword,        MakeSwordTool());
        setTool(Items::DiamondSword,     MakeSwordTool());
        setTool(Items::NetheriteSword,   MakeSwordTool());
        setTool(Items::ResoniteSword,    MakeSwordTool());
        // Shears — ShearsItem.createToolProperties.
        // No useOn wired: ShearsItem's interactions (beehive honeycombs,
        // pumpkin carving) all produce item DROPS — BLOCKED on item entities.
        setTool(Items::Shears,           MakeShearsTool());

        // The Aether (AetherItemTiers) and Twilight Forest (TFToolMaterials)
        // tool sets, docs/mod-ports.md. Their incorrect-for-drops tags are the
        // vanilla tiers' (skyroot = wooden, holystone = stone, zanite = iron,
        // gravitite = diamond; ironwood = iron, steeleaf / knightmetal =
        // diamond, fiery = netherite), so they need no new MiningTier; the
        // speed is the material's own. The steeleaf sword is registered on
        // KNIGHTMETAL in TFItems, which has the same speed.
        setTool(Items::SkyrootPickaxe,        MakeDiggerTool(ToolType::Pickaxe, MiningTier::Wood, 2.0f));
        setTool(Items::SkyrootAxe,            MakeDiggerTool(ToolType::Axe, MiningTier::Wood, 2.0f));
        setTool(Items::SkyrootShovel,         MakeDiggerTool(ToolType::Shovel, MiningTier::Wood, 2.0f));
        setTool(Items::SkyrootHoe,            MakeDiggerTool(ToolType::Hoe, MiningTier::Wood, 2.0f));
        setTool(Items::SkyrootSword,          MakeSwordTool());
        setTool(Items::HolystonePickaxe,      MakeDiggerTool(ToolType::Pickaxe, MiningTier::Stone, 4.0f));
        setTool(Items::HolystoneAxe,          MakeDiggerTool(ToolType::Axe, MiningTier::Stone, 4.0f));
        setTool(Items::HolystoneShovel,       MakeDiggerTool(ToolType::Shovel, MiningTier::Stone, 4.0f));
        setTool(Items::HolystoneHoe,          MakeDiggerTool(ToolType::Hoe, MiningTier::Stone, 4.0f));
        setTool(Items::HolystoneSword,        MakeSwordTool());
        setTool(Items::ZanitePickaxe,         MakeDiggerTool(ToolType::Pickaxe, MiningTier::Iron, 6.0f));
        setTool(Items::ZaniteAxe,             MakeDiggerTool(ToolType::Axe, MiningTier::Iron, 6.0f));
        setTool(Items::ZaniteShovel,          MakeDiggerTool(ToolType::Shovel, MiningTier::Iron, 6.0f));
        setTool(Items::ZaniteHoe,             MakeDiggerTool(ToolType::Hoe, MiningTier::Iron, 6.0f));
        setTool(Items::ZaniteSword,           MakeSwordTool());
        setTool(Items::GravititePickaxe,      MakeDiggerTool(ToolType::Pickaxe, MiningTier::Diamond, 8.0f));
        setTool(Items::GravititeAxe,          MakeDiggerTool(ToolType::Axe, MiningTier::Diamond, 8.0f));
        setTool(Items::GravititeShovel,       MakeDiggerTool(ToolType::Shovel, MiningTier::Diamond, 8.0f));
        setTool(Items::GravititeHoe,          MakeDiggerTool(ToolType::Hoe, MiningTier::Diamond, 8.0f));
        setTool(Items::GravititeSword,        MakeSwordTool());
        setTool(Items::IronwoodPickaxe,       MakeDiggerTool(ToolType::Pickaxe, MiningTier::Iron, 6.5f));
        setTool(Items::IronwoodAxe,           MakeDiggerTool(ToolType::Axe, MiningTier::Iron, 6.5f));
        setTool(Items::IronwoodShovel,        MakeDiggerTool(ToolType::Shovel, MiningTier::Iron, 6.5f));
        setTool(Items::IronwoodHoe,           MakeDiggerTool(ToolType::Hoe, MiningTier::Iron, 6.5f));
        setTool(Items::IronwoodSword,         MakeSwordTool());
        setTool(Items::SteeleafPickaxe,       MakeDiggerTool(ToolType::Pickaxe, MiningTier::Diamond, 8.0f));
        setTool(Items::SteeleafAxe,           MakeDiggerTool(ToolType::Axe, MiningTier::Diamond, 8.0f));
        setTool(Items::SteeleafShovel,        MakeDiggerTool(ToolType::Shovel, MiningTier::Diamond, 8.0f));
        setTool(Items::SteeleafHoe,           MakeDiggerTool(ToolType::Hoe, MiningTier::Diamond, 8.0f));
        setTool(Items::SteeleafSword,         MakeSwordTool());
        setTool(Items::KnightmetalPickaxe,    MakeDiggerTool(ToolType::Pickaxe, MiningTier::Diamond, 8.0f));
        setTool(Items::KnightmetalAxe,        MakeDiggerTool(ToolType::Axe, MiningTier::Diamond, 8.0f));
        setTool(Items::KnightmetalSword,      MakeSwordTool());
        setTool(Items::FieryPickaxe,          MakeDiggerTool(ToolType::Pickaxe, MiningTier::Netherite, 9.0f));
        setTool(Items::FierySword,            MakeSwordTool());

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
            // The music discs (Items.java:3122-3144): UNCOMMON, except the
            // four RARE ones — creator, lava_chicken, otherside, pigstep —
            // and the fragment, UNCOMMON.
            for (ItemID disc : { Items::MusicDisc13, Items::MusicDiscCat, Items::MusicDiscBlocks,
                                 Items::MusicDiscBounce, Items::MusicDiscChirp,
                                 Items::MusicDiscCreatorMusicBox, Items::MusicDiscFar,
                                 Items::MusicDiscMall, Items::MusicDiscMellohi, Items::MusicDiscStal,
                                 Items::MusicDiscStrad, Items::MusicDiscWard, Items::MusicDisc11,
                                 Items::MusicDiscWait, Items::MusicDiscRelic, Items::MusicDisc5,
                                 Items::MusicDiscPrecipice, Items::MusicDiscTears,
                                 Items::DiscFragment5 }) {
                setRarity(disc, Rarity::UNCOMMON);
            }
            for (ItemID disc : { Items::MusicDiscCreator, Items::MusicDiscLavaChicken,
                                 Items::MusicDiscOtherside, Items::MusicDiscPigstep }) {
                setRarity(disc, Rarity::RARE);
            }
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

        // The ominous bottle's drink and Bad Omen level (TrialItems.cpp).
        ItemRegistry_RegisterTrialItems(pureItems);

        // BoatItem.use and MinecartItem.useOn (vehicle/VehicleItems.cpp).
        ItemRegistry_RegisterVehicleItems(pureItems);

        Log::Info("[ItemRegistry] Wired use-behaviour callbacks "
                  "(FlintAndSteel, 8 hoes, 8 shovels) + Tool components on 41 tool items");
    }

} // namespace Game
