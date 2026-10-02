// File: src/common/world/block/WitherSkullBlock.cpp
//
// See WitherSkullBlock.hpp. Every function names the MC method it ports.
#include "common/world/block/WitherSkullBlock.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/CarvedPumpkinBlock.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/pattern/BlockPattern.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/tags/DataTags.hpp"
#include "server/advancements/CriteriaTriggers.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace Game {

    namespace {

        // BlockStatePredicate.forBlock(WITHER_SKELETON_SKULL)
        // .or(forBlock(WITHER_SKELETON_WALL_SKULL)).
        bool IsWitherSkullBlock(BlockState state) {
            return state.Block() == BlockID::WitherSkeletonSkull ||
                   state.Block() == BlockID::WitherSkeletonWallSkull;
        }

        // block.getState().is(BlockTags.WITHER_SUMMON_BASE_BLOCKS).
        bool IsWitherSummonBase(BlockState state) {
            return DataTags::HasTag(DataTags::Registry::Block, BlockRegistry::Get(state.Block()).registrySlug,
                                    "minecraft:wither_summon_base_blocks");
        }

        // block.getState().isAir().
        bool IsAirState(BlockState state) { return state.Block() == BlockID::Air; }

        // MC WitherSkullBlock.getOrCreateWitherFull.
        const BlockPattern& WitherPatternFull() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({"^^^", "###", "~#~"})
                .Where('#', BlockInWorld::HasState(&IsWitherSummonBase))
                .Where('^', BlockInWorld::HasState(&IsWitherSkullBlock))
                .Where('~', BlockInWorld::HasState(&IsAirState))
                .Build();
            return pattern;
        }

        // MC WitherSkullBlock.getOrCreateWitherBase.
        const BlockPattern& WitherPatternBase() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({"   ", "###", "~#~"})
                .Where('#', BlockInWorld::HasState(&IsWitherSummonBase))
                .Where('~', BlockInWorld::HasState(&IsAirState))
                .Build();
            return pattern;
        }

        // WitherSkullBlock.setPlacedBy / WitherWallSkullBlock.setPlacedBy.
        void WitherSkullSetPlacedBy(ILevelWrite& level, const glm::ivec3& pos, BlockState /*state*/,
                                    IUsePlayer* /*by*/, const ItemStack& /*stack*/) {
            WitherSkullCheckSpawn(level, pos);
        }

    } // namespace

    void WitherSkullCheckSpawn(ILevelWrite& level, const glm::ivec3& pos) {
        // MC checkSpawn(level, pos): `level.getBlockEntity(pos) instanceof
        // SkullBlockEntity` — the skull's own state is what is tested below,
        // so a cell that no longer holds one never matches.
        if (level.IsClientSide()) return;
        EntityLevel* entities = level.Entities();
        if (!entities) return;
        const BlockState placed = level.GetBlockState(pos.x, pos.y, pos.z);
        const bool correctBlock = IsWitherSkullBlock(placed);
        if (!correctBlock || pos.y < entities->GetMinY() ||
            entities->GetDifficulty() == Difficulty::Peaceful) {
            return;
        }

        const std::optional<BlockPattern::Match> match = WitherPatternFull().Find(level, pos);
        if (!match) return;

        // EntityTypes.WITHER.create(level, TRIGGERED) — no finalizeSpawn.
        auto witherBoss = std::make_unique<Wither>(entities);
        ClearPatternBlocks(level, *match);

        // snapTo(spawn + (0.5, 0.55, 0.5), forwards axis X ? 0 : 90, 0);
        // yBodyRot the same.
        const glm::ivec3 spawnPos = match->GetBlock(1, 2, 0).GetPos();
        const float yaw = AxisOf(match->GetForwards()) == Axis::X ? 0.0f : 90.0f;
        witherBoss->position = glm::dvec3(static_cast<double>(spawnPos.x) + 0.5,
                                          static_cast<double>(spawnPos.y) + 0.55,
                                          static_cast<double>(spawnPos.z) + 0.5);
        witherBoss->yRot = yaw;
        witherBoss->xRot = 0.0f;
        witherBoss->yBodyRot = yaw;
        witherBoss->SetOldPosAndRot();
        witherBoss->MakeInvulnerable();

        // level.getEntitiesOfClass(ServerPlayer.class, box.inflate(50)) —
        // box overlap, NO_SPECTATORS (getEntitiesOfClass's default).
        AABBd box = witherBoss->GetAABBd();
        box.min -= glm::dvec3(50.0);
        box.max += glm::dvec3(50.0);
        std::vector<LivingEntity*> players;
        entities->GetPlayers(players);
        for (LivingEntity* player : players) {
            if (!player || player->IsSpectator()) continue;
            if (!player->GetAABBd().Intersects(box)) continue;
            if (Server::ServerPlayer* serverPlayer = Server::CriteriaTriggers::PlayerOf(player)) {
                Server::CriteriaTriggers::SummonedEntity(*serverPlayer, *witherBoss);
            }
        }

        entities->AddFreshEntity(std::move(witherBoss));
        UpdatePatternBlocks(level, *match);
    }

    bool WitherSkullCanSpawnMob(ILevelWrite& level, const glm::ivec3& pos, const ItemStack& stack) {
        if (stack.IsEmpty() || stack.itemId != ItemRegistry::FromBlock(BlockID::WitherSkeletonSkull)) return false;
        if (level.IsClientSide()) return false;
        EntityLevel* entities = level.Entities();
        if (!entities) return false;
        if (pos.y < entities->GetMinY() + 2 || entities->GetDifficulty() == Difficulty::Peaceful) return false;
        return WitherPatternBase().Find(level, pos).has_value();
    }

    void RegisterWitherSkullBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        blocks[static_cast<size_t>(BlockID::WitherSkeletonSkull)].setPlacedBy     = &WitherSkullSetPlacedBy;
        blocks[static_cast<size_t>(BlockID::WitherSkeletonWallSkull)].setPlacedBy = &WitherSkullSetPlacedBy;
    }

} // namespace Game
