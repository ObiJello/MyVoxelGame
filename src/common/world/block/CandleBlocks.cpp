// File: src/common/world/block/CandleBlocks.cpp
//
// See CandleBlocks.hpp. Every function names the MC method it ports.
#include "common/world/block/CandleBlocks.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/LegacySolid.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <glm/glm.hpp>

namespace Game::Candles {

    namespace {

        // CandleCakeBlock.BY_CANDLE, as the pairs Blocks.java registers them.
        struct CandlePair { BlockID candle; BlockID cake; };
        constexpr CandlePair kCandlePairs[] = {
            { BlockID::Candle,          BlockID::CandleCake },
            { BlockID::WhiteCandle,     BlockID::WhiteCandleCake },
            { BlockID::OrangeCandle,    BlockID::OrangeCandleCake },
            { BlockID::MagentaCandle,   BlockID::MagentaCandleCake },
            { BlockID::LightBlueCandle, BlockID::LightBlueCandleCake },
            { BlockID::YellowCandle,    BlockID::YellowCandleCake },
            { BlockID::LimeCandle,      BlockID::LimeCandleCake },
            { BlockID::PinkCandle,      BlockID::PinkCandleCake },
            { BlockID::GrayCandle,      BlockID::GrayCandleCake },
            { BlockID::LightGrayCandle, BlockID::LightGrayCandleCake },
            { BlockID::CyanCandle,      BlockID::CyanCandleCake },
            { BlockID::PurpleCandle,    BlockID::PurpleCandleCake },
            { BlockID::BlueCandle,      BlockID::BlueCandleCake },
            { BlockID::BrownCandle,     BlockID::BrownCandleCake },
            { BlockID::GreenCandle,     BlockID::GreenCandleCake },
            { BlockID::RedCandle,       BlockID::RedCandleCake },
            { BlockID::BlackCandle,     BlockID::BlackCandleCake },
        };

        // CandleBlock.PARTICLE_OFFSETS (block-local, already × 1/16) and
        // CandleCakeBlock.PARTICLE_OFFSETS — the wick tips.
        struct WickSet { int count; glm::dvec3 at[4]; };
        constexpr double kPx = 1.0 / 16.0;
        const WickSet kCandleWicks[4] = {
            { 1, { {8 * kPx, 8 * kPx, 8 * kPx} } },
            { 2, { {6 * kPx, 7 * kPx, 8 * kPx}, {10 * kPx, 8 * kPx, 7 * kPx} } },
            { 3, { {8 * kPx, 5 * kPx, 10 * kPx}, {6 * kPx, 7 * kPx, 8 * kPx}, {9 * kPx, 8 * kPx, 7 * kPx} } },
            { 4, { {7 * kPx, 5 * kPx, 9 * kPx}, {10 * kPx, 7 * kPx, 9 * kPx},
                   {6 * kPx, 7 * kPx, 6 * kPx}, {9 * kPx, 8 * kPx, 6 * kPx} } },
        };
        const WickSet kCakeWick = { 1, { {8 * kPx, 16 * kPx, 8 * kPx} } };

        const WickSet* WicksOf(BlockState state) {
            const BlockID id = state.Block();
            if (IsCandleCake(id)) return &kCakeWick;
            const int n = CandleCount(state);
            return n >= 1 && n <= 4 ? &kCandleWicks[n - 1] : nullptr;
        }

        bool IsWaterlogged(BlockState state) {
            return state.HasProperty(PropertyId::WATERLOGGED) &&
                   state.GetName(PropertyId::WATERLOGGED) == "true";
        }

        // MC `itemStack.consume(1, player)`: nothing is used up with infinite
        // materials (creative).
        void ConsumeOne(ItemStack& stack, const IUsePlayer* player) {
            if (player && player->isCreative()) return;
            if (stack.count > 0) --stack.count;
            if (stack.count <= 0) stack.Clear();
        }

        // ── CakeBlock ─────────────────────────────────────────────────────

        // MC CakeBlock.eat(level, pos, state, player). `state` is the CAKE
        // state to take a bite out of — a candle cake passes the uneaten
        // Blocks.CAKE default, which is how its first slice leaves a cake
        // with one bite gone.
        UseResult CakeEat(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                          IUsePlayer* player) {
            if (!player || !player->CanEat(false)) return UseResult::Pass;
            // player.awardStat(EAT_CAKE_SLICE); player.getFoodData().eat(2, 0.1F)
            player->EatFood(2, 0.1f);
            const int bites = state.GetIndex(PropertyId::BITES);   // values run 0..6
            // level.gameEvent(player, GameEvent.EAT, pos).
            Entity* eater = player->GameEventSource();
            level.GameEvent(eater, GameEventId::Eat, pos);
            if (bites < 6) {
                level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::BITES, bites + 1),
                               World::UpdateFlags::All);
            } else {
                // level.removeBlock(pos, false), then gameEvent(player,
                // BLOCK_DESTROY, pos).
                level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
                level.GameEvent(eater, GameEventId::BlockDestroy, pos);
            }
            return UseResult::Success;
        }

        // MC CakeBlock.useItemOn: a candle on an uneaten cake.
        UseResult CakeUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                IUsePlayer* player, uint32_t /*hand*/,
                                const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            const BlockID held = ItemRegistry::IsBlockItem(stack.itemId)
                                     ? ItemRegistry::ToBlock(stack.itemId) : BlockID::Air;
            // itemStack.is(ItemTags.CANDLES) && bites == 0, and the item's
            // block is a CandleBlock (all seventeen are).
            if (!stack.IsEmpty() && IsCandle(held) && state.GetIndex(PropertyId::BITES) == 0) {
                ConsumeOne(stack, player);
                level->PlaySound(nullptr, pos, SoundEvents::CAKE_ADD_CANDLE, SoundSource::Blocks,
                                 1.0f, 1.0f);
                level->SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(CandleCakeOf(held)),
                                World::UpdateFlags::All);
                // level.gameEvent(player, BLOCK_CHANGE, pos); (awardStat(ITEM_USED))
                level->GameEvent(player ? player->GameEventSource() : nullptr, GameEventId::BlockChange, pos);
                return UseResult::Success;
            }
            return UseResult::TryEmptyHandInteraction;
        }

        // MC CakeBlock.useWithoutItem.
        UseResult CakeUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos,
                                     IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (level->IsClientSide()) {
                if (ConsumesAction(CakeEat(*level, pos, state, player))) return UseResult::Success;
                if (player && player->getItemInHand(0).IsEmpty()) return UseResult::Consume;
            }
            return CakeEat(*level, pos, state, player);
        }

        // MC CakeBlock.updateShape / CandleCakeBlock.updateShape: a cake whose
        // support below stops being solid is gone.
        bool CakeUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState /*state*/,
                             Direction toNeighbour, BlockID /*neighbourId*/,
                             BlockState& outState, ScheduledTickAccess* /*ticks*/) {
            if (toNeighbour != Direction::Down || CakeCanSurvive(level, pos)) return false;
            outState = BlockState{};
            return true;
        }

        // ── CandleBlock ───────────────────────────────────────────────────

        // MC CandleBlock.useItemOn: an empty hand puts a lit candle out.
        UseResult CandleUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                  IUsePlayer* player, uint32_t /*hand*/,
                                  const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (stack.IsEmpty() && IsLit(state)) {
                Extinguish(*level, pos, state, player ? player->GameEventSource() : nullptr);
                return UseResult::Success;
            }
            // super.useItemOn → TRY_WITH_EMPTY_HAND. A candle has no
            // useWithoutItem, so the dispatch carries on to the item — which
            // is how a candle in hand grows the clump (BlockItem placement).
            return UseResult::TryEmptyHandInteraction;
        }

        // ── CandleCakeBlock ───────────────────────────────────────────────

        // MC CandleCakeBlock.candleHit: the click landed on the candle half.
        bool CandleHit(const BlockHitResult& hit) {
            return hit.hitPoint.y - static_cast<double>(hit.blockPos.y) > 0.5;
        }

        // MC CandleCakeBlock.useItemOn.
        UseResult CandleCakeUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                      IUsePlayer* player, uint32_t /*hand*/,
                                      const BlockHitResult& hit) {
            if (!level) return UseResult::Pass;
            // Flint and steel / fire charge: PASS, so the item's useOn lights it.
            if (stack.itemId == Items::FlintAndSteel || stack.itemId == Items::FireCharge) {
                return UseResult::Pass;
            }
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            if (CandleHit(hit) && stack.IsEmpty() && IsLit(state)) {
                Extinguish(*level, pos, state, player ? player->GameEventSource() : nullptr);
                return UseResult::Success;
            }
            return UseResult::TryEmptyHandInteraction;
        }

        // MC CandleCakeBlock.useWithoutItem: a slice of the cake under the
        // candle — the block becomes a cake with one bite taken and the
        // candle drops (the candle cake's loot table).
        UseResult CandleCakeUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos,
                                           IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
            const UseResult eaten = CakeEat(*level, pos, BlockStates::Default(BlockID::Cake), player);
            // Block.dropResources is a ServerLevel-only call.
            if (ConsumesAction(eaten) && !level->IsClientSide()) {
                DropBlockLoot(*level, pos, state);
            }
            return eaten;
        }

        // MC CandleCakeBlock.getAnalogOutputSignal: CakeBlock.FULL_CAKE_SIGNAL
        // — a candle cake is always an uneaten cake to a comparator.
        int CandleCakeAnalogOutput(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                   BlockState /*state*/, Direction /*direction*/) {
            return 14;
        }

        // MC AbstractCandleBlock.onProjectileHit: a burning projectile lights
        // whatever canBeLit allows (CandleBlock adds the dry-wick test).
        void CandleOnProjectileHit(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   const glm::dvec3& /*hitPos*/, Direction /*face*/,
                                   Entity& projectile) {
            if (level.IsClientSide() || !projectile.IsOnFire()) return;
            const bool canBeLit = IsCandle(state.Block()) ? CandleCanLight(state)
                                                          : CandleCakeCanLight(state);
            if (!canBeLit) return;
            // AbstractCandleBlock.setLit: flags 11.
            level.SetBlock(pos.x, pos.y, pos.z, WithLit(state, true), World::UpdateFlags::All);
        }

    } // namespace

    bool IsCandle(BlockID id) {
        for (const CandlePair& p : kCandlePairs) if (p.candle == id) return true;
        return false;
    }

    bool IsCandleCake(BlockID id) {
        for (const CandlePair& p : kCandlePairs) if (p.cake == id) return true;
        return false;
    }

    BlockID CandleCakeOf(BlockID candle) {
        for (const CandlePair& p : kCandlePairs) if (p.candle == candle) return p.cake;
        return BlockID::Air;
    }

    bool IsLit(BlockState state) {
        const BlockID id = state.Block();
        return (IsCandle(id) || IsCandleCake(id)) && state.HasProperty(PropertyId::LIT) &&
               state.GetName(PropertyId::LIT) == "true";
    }

    bool CandleCanLight(BlockState state) {
        // state.is(#candles) with LIT and WATERLOGGED, && !lit && !waterlogged.
        return IsCandle(state.Block()) && state.HasProperty(PropertyId::LIT) &&
               state.HasProperty(PropertyId::WATERLOGGED) && !IsLit(state) && !IsWaterlogged(state);
    }

    bool CandleCakeCanLight(BlockState state) {
        return IsCandleCake(state.Block()) && state.HasProperty(PropertyId::LIT) && !IsLit(state);
    }

    int CandleCount(BlockState state) {
        if (!IsCandle(state.Block()) || !state.HasProperty(PropertyId::CANDLES)) return 0;
        const int index = state.GetIndex(PropertyId::CANDLES);   // values run 1..4
        return index < 0 ? 0 : index + 1;
    }

    BlockState WithLit(BlockState state, bool lit) {
        if (!state.HasProperty(PropertyId::LIT)) return state;
        return state.SetName(PropertyId::LIT, lit ? "true" : "false");
    }

    void Extinguish(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Entity* player) {
        // setLit(level, state, pos, false) — flags 11.
        level.SetBlock(pos.x, pos.y, pos.z, WithLit(state, false), World::UpdateFlags::All);
        // A SMOKE puff rising (0, 0.1, 0) from every wick. Level.addParticle:
        // only a client level draws it, so this is the acting player's
        // prediction, exactly as in vanilla.
        if (const WickSet* wicks = WicksOf(state)) {
            for (int i = 0; i < wicks->count; ++i) {
                const glm::dvec3 at = glm::dvec3(pos) + wicks->at[i];
                level.AddParticle(ParticleKind::Smoke, at.x, at.y, at.z, 0.0, 0.1, 0.0);
            }
        }
        // level.playSound(null, pos, CANDLE_EXTINGUISH, BLOCKS, 1.0F, 1.0F)
        level.PlaySound(nullptr, pos, SoundEvents::CANDLE_EXTINGUISH, SoundSource::Blocks, 1.0f, 1.0f);
        // level.gameEvent(player, BLOCK_CHANGE, pos).
        level.GameEvent(player, GameEventId::BlockChange, pos);
    }

    bool CandleCanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
        // Block.canSupportCenter(level, below, UP) =
        //   state.isFaceSturdy(level, below, UP, SupportType.CENTER):
        //   the UP face of getBlockSupportShape covers Block.column(2, 0, 10)
        //   — the 7..9 pixel square in the middle.
        const glm::ivec3 below{pos.x, pos.y - 1, pos.z};
        const BlockState state = level.GetBlockState(below.x, below.y, below.z);
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;
        const std::string& slug = BlockRegistry::Get(id).registrySlug;
        // The getBlockSupportShape overrides that decide it outright:
        // LeavesBlock → empty, SoulSandBlock / MudBlock → the full block.
        if (slug.size() > 7 && slug.compare(slug.size() - 7, 7, "_leaves") == 0) return false;
        if (id == BlockID::SoulSand || id == BlockID::Mud) return true;
        // Default support shape = the collision shape, so a no-collision
        // block (a flower, a torch) supports nothing. Scaffolding is the one
        // exception: its empty-context collision is the stable shape.
        if (!BlockRegistry::HasCollision(id) && id != BlockID::Scaffolding) return false;
        // VoxelShape.getFaceShape(UP) is the slice at y = 0.9999999: every
        // box that spans it — a slab top, a fence post, a full cube.
        constexpr float kSlice = 0.9999999f;
        constexpr float kLo = 7.0f / 16.0f + 1e-5f, kHi = 9.0f / 16.0f - 1e-5f;
        for (const auto& box : BlockRegistry::GetBlockShapeSet(state)) {
            if (box.min.y <= kSlice && box.max.y >= kSlice &&
                box.min.x <= kLo && box.max.x >= kHi && box.min.z <= kLo && box.max.z >= kHi) {
                return true;
            }
        }
        return false;
    }

    bool CakeCanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
        return IsLegacySolid(level.GetBlockState(pos.x, pos.y - 1, pos.z));
    }

    bool CandlePlaceLiquid(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        if (!IsCandle(state.Block()) || IsWaterlogged(state) || !IsLit(state)) return false;
        if (level.IsClientSide()) return true;
        // BlockState newState = state.setValue(WATERLOGGED, true);
        // if (LIT) extinguish(null, newState, level, pos);
        Extinguish(level, pos, BlockRegistry::WithWaterlogged(state, true));
        return true;
    }

} // namespace Game::Candles

namespace Game {

    void RegisterCandleBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        using namespace Candles;
        for (const CandlePair& p : kCandlePairs) {
            Block& candle = blocks[static_cast<size_t>(p.candle)];
            candle.useItemOn       = &CandleUseItemOn;
            candle.onProjectileHit = &CandleOnProjectileHit;
            // No updateShape beyond the shared waterlogged tick: CandleBlock
            // checks canSurvive at placement only, so a candle stays put when
            // the block under it goes, exactly as in vanilla.

            Block& cake = blocks[static_cast<size_t>(p.cake)];
            cake.useItemOn       = &CandleCakeUseItemOn;
            cake.useWithoutItem  = &CandleCakeUseWithoutItem;
            cake.onProjectileHit = &CandleOnProjectileHit;
            cake.updateShape     = &CakeUpdateShape;
            cake.hasAnalogOutputSignal = true;
            cake.getAnalogOutputSignal = &CandleCakeAnalogOutput;
        }
        Block& cake = blocks[static_cast<size_t>(BlockID::Cake)];
        cake.useItemOn      = &CakeUseItemOn;
        cake.useWithoutItem = &CakeUseWithoutItem;
        cake.updateShape    = &CakeUpdateShape;
    }

} // namespace Game
