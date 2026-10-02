// File: src/common/world/block/CarvedPumpkinBlock.cpp
//
// See CarvedPumpkinBlock.hpp. Every function names the MC method it ports.
#include "common/world/block/CarvedPumpkinBlock.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/DoubleChest.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"
#include "server/advancements/CriteriaTriggers.hpp"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Game {

    namespace {

        // MC CarvedPumpkinBlock.PUMPKINS_PREDICATE.
        bool IsPumpkinHead(BlockState state) {
            return state.Block() == BlockID::CarvedPumpkin || state.Block() == BlockID::JackOLantern;
        }

        // BlockStatePredicate.forBlock(SNOW_BLOCK) / forBlock(IRON_BLOCK).
        bool IsSnowBlock(BlockState state) { return state.Block() == BlockID::Snow; }
        bool IsIronBlock(BlockState state) { return state.Block() == BlockID::IronBlock; }

        // BlockBehaviour.BlockStateBase::isAir.
        bool IsAirState(BlockState state) { return state.Block() == BlockID::Air; }

        // block.is(BlockTags.COPPER) — data/minecraft/tags/block/copper.json:
        // the four weathering blocks of copper and their waxed twins.
        bool IsCopperTagged(BlockState state) {
            return DataTags::HasTag(DataTags::Registry::Block, BlockRegistry::Get(state.Block()).registrySlug,
                                    "minecraft:copper");
        }

        // ── The patterns (getOrCreate*) ─────────────────────────────────
        //
        // MC builds each lazily on the block instance; here once per process
        // (function-local statics — thread-safe initialisation).

        const BlockPattern& SnowGolemBase() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({" ", "#", "#"})
                .Where('#', BlockInWorld::HasState(&IsSnowBlock))
                .Build();
            return pattern;
        }

        const BlockPattern& SnowGolemFull() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({"^", "#", "#"})
                .Where('^', BlockInWorld::HasState(&IsPumpkinHead))
                .Where('#', BlockInWorld::HasState(&IsSnowBlock))
                .Build();
            return pattern;
        }

        const BlockPattern& IronGolemBase() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({"~ ~", "###", "~#~"})
                .Where('#', BlockInWorld::HasState(&IsIronBlock))
                .Where('~', BlockInWorld::HasState(&IsAirState))
                .Build();
            return pattern;
        }

        const BlockPattern& IronGolemFull() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({"~^~", "###", "~#~"})
                .Where('^', BlockInWorld::HasState(&IsPumpkinHead))
                .Where('#', BlockInWorld::HasState(&IsIronBlock))
                .Where('~', BlockInWorld::HasState(&IsAirState))
                .Build();
            return pattern;
        }

        const BlockPattern& CopperGolemBase() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({" ", "#"})
                .Where('#', BlockInWorld::HasState(&IsCopperTagged))
                .Build();
            return pattern;
        }

        const BlockPattern& CopperGolemFull() {
            static const BlockPattern pattern = BlockPatternBuilder::Start()
                .Aisle({"^", "#"})
                .Where('^', BlockInWorld::HasState(&IsPumpkinHead))
                .Where('#', BlockInWorld::HasState(&IsCopperTagged))
                .Build();
            return pattern;
        }

        // The state a matched cell held when the pattern was found (the
        // match's cache keeps it, so it survives clearPatternBlocks).
        BlockState MatchedState(BlockInWorld& block) {
            return block.GetState().value_or(BlockStates::Default(BlockID::Air));
        }

        // MC CarvedPumpkinBlock.spawnGolemInWorld: clear the pattern, put the
        // golem at the spawn cell's bottom centre (+0.05) with yaw and pitch
        // 0, add it, award SUMMONED_ENTITY to every player near it, then run
        // the deferred neighbour updates. Returns the golem (owned by the
        // level from here on) for the caller's follow-up.
        Mob* SpawnGolemInWorld(ILevelWrite& level, EntityLevel& entities, const BlockPattern::Match& match,
                               std::unique_ptr<Mob> golem, const glm::ivec3& spawnPos) {
            ClearPatternBlocks(level, match);

            // golem.snapTo(x + 0.5, y + 0.05, z + 0.5, 0, 0).
            golem->position = glm::dvec3(static_cast<double>(spawnPos.x) + 0.5,
                                         static_cast<double>(spawnPos.y) + 0.05,
                                         static_cast<double>(spawnPos.z) + 0.5);
            golem->yRot = 0.0f;
            golem->xRot = 0.0f;
            golem->SetOldPosAndRot();

            // level.addFreshEntity(golem). The level keeps it alive from here;
            // the raw pointer stays valid (the spawn queue holds it until the
            // mob manager absorbs it).
            Mob* spawned = golem.get();
            entities.AddFreshEntity(std::move(golem));

            // level.getEntitiesOfClass(ServerPlayer.class,
            // golem.getBoundingBox().inflate(5.0)) — box overlap, and
            // EntitySelector.NO_SPECTATORS (getEntitiesOfClass's default).
            AABBd box = spawned->GetAABBd();
            box.min -= glm::dvec3(5.0);
            box.max += glm::dvec3(5.0);
            std::vector<LivingEntity*> players;
            entities.GetPlayers(players);
            for (LivingEntity* player : players) {
                if (!player || player->IsSpectator()) continue;
                if (!player->GetAABBd().Intersects(box)) continue;
                if (Server::ServerPlayer* serverPlayer = Server::CriteriaTriggers::PlayerOf(player)) {
                    Server::CriteriaTriggers::SummonedEntity(*serverPlayer, *spawned);
                }
            }

            UpdatePatternBlocks(level, match);
            return spawned;
        }

        // MC ChestBlock.candidatePartnerFacing, as getChestType asks it: the
        // facing of a SINGLE chest `chestBlock` can connect to at `at`.
        std::optional<Direction> CandidatePartnerFacing(const IBlockAccess& level, BlockID chestBlock,
                                                        const glm::ivec3& at) {
            const BlockState state = level.GetBlockState(at.x, at.y, at.z);
            if (!ChestCanConnectTo(chestBlock, state.Block())) return std::nullopt;
            if (state.GetValueByName("type") != "single") return std::nullopt;
            return HorizontalFacingOf(state);
        }

        // MC CopperChestBlock.getFromCopperBlock(copperBlock, facing, level,
        // pos): the copper chest of the copper block's oxidation and wax
        // (COPPER_TO_COPPER_CHEST_MAPPING, the plain copper chest for
        // anything else), facing `facing`, typed by ChestBlock.getChestType
        // — LEFT beside a lone same-facing copper chest on its clockwise
        // side, else RIGHT for one counter-clockwise, else SINGLE — and then
        // getLeastOxidizedChestOfConnectedBlocks.
        BlockState CopperChestFromCopperBlock(BlockID copperBlock, Direction facing, const IBlockAccess& level,
                                              const glm::ivec3& pos) {
            // WeatheringCopperCollection.zipApply(COPPER_BLOCK, COPPER_CHEST).
            static constexpr BlockID kUnwaxed[4] = {BlockID::CopperBlock, BlockID::ExposedCopper,
                                                    BlockID::WeatheredCopper, BlockID::OxidizedCopper};
            static constexpr BlockID kWaxed[4] = {BlockID::WaxedCopperBlock, BlockID::WaxedExposedCopper,
                                                  BlockID::WaxedWeatheredCopper, BlockID::WaxedOxidizedCopper};
            BlockID chestBlock = CopperChestOf(0, false);
            for (int stage = 0; stage < 4; ++stage) {
                if (copperBlock == kUnwaxed[stage]) chestBlock = CopperChestOf(stage, false);
                if (copperBlock == kWaxed[stage])   chestBlock = CopperChestOf(stage, true);
            }

            // ChestBlock.getChestType(level, pos, facing).
            std::string_view type = "single";
            const auto partnerAt = [&pos](Direction d) {
                return glm::ivec3(pos.x + StepX(d), pos.y + StepY(d), pos.z + StepZ(d));
            };
            if (CandidatePartnerFacing(level, chestBlock, partnerAt(ClockWise(facing))) == facing) {
                type = "left";
            } else if (CandidatePartnerFacing(level, chestBlock, partnerAt(CounterClockWise(facing))) == facing) {
                type = "right";
            }

            const BlockState state = WithHorizontalFacing(BlockStates::Default(chestBlock), facing)
                                         .SetName(PropertyId::CHEST_TYPE, type);
            return CopperChestLeastOxidizedState(level, pos, state);
        }

        // MC CarvedPumpkinBlock.replaceCopperBlockWithChest: the copper block
        // under the head becomes a copper chest facing the way the head did,
        // written with flag 2.
        void ReplaceCopperBlockWithChest(ILevelWrite& level, const BlockPattern::Match& match) {
            BlockInWorld& copperBlock  = match.GetBlock(0, 1, 0);
            BlockInWorld& pumpkinBlock = match.GetBlock(0, 0, 0);
            const Direction facing = HorizontalFacingOf(MatchedState(pumpkinBlock));
            const glm::ivec3& at = copperBlock.GetPos();
            const BlockState chest = CopperChestFromCopperBlock(MatchedState(copperBlock).Block(), facing, level, at);
            level.SetBlock(at.x, at.y, at.z, chest, World::UpdateFlags::UpdateClients);
        }

        // MC CarvedPumpkinBlock.getWeatherStateFromPattern: the copper
        // block's own age (a WeatheringCopper), else the age of what its wax
        // comes off to (HoneycombItem.WAX_OFF_BY_BLOCK), else UNAFFECTED. The
        // match's cache keeps the block it held before the chest replaced it.
        CopperGolem::WeatherState WeatherStateFromPattern(const BlockPattern::Match& match) {
            switch (MatchedState(match.GetBlock(0, 1, 0)).Block()) {
                case BlockID::ExposedCopper:
                case BlockID::WaxedExposedCopper:   return CopperGolem::WeatherState::Exposed;
                case BlockID::WeatheredCopper:
                case BlockID::WaxedWeatheredCopper: return CopperGolem::WeatherState::Weathered;
                case BlockID::OxidizedCopper:
                case BlockID::WaxedOxidizedCopper:  return CopperGolem::WeatherState::Oxidized;
                default:                            return CopperGolem::WeatherState::Unaffected;
            }
        }

        // MC CarvedPumpkinBlock.trySpawnGolem: snow golem, else iron golem,
        // else copper golem — the first full pattern found wins.
        void TrySpawnGolem(ILevelWrite& level, const glm::ivec3& topPos) {
            // EntityTypes.X.create(level, TRIGGERED) — no level to create
            // into means no golem (MC: create answers null).
            EntityLevel* entities = level.Entities();
            if (!entities) return;

            if (const auto snowGolemMatch = SnowGolemFull().Find(level, topPos)) {
                // Spawns at the bottom snow block (getBlock(0, 2, 0)).
                SpawnGolemInWorld(level, *entities, *snowGolemMatch, std::make_unique<SnowGolem>(entities),
                                  snowGolemMatch->GetBlock(0, 2, 0).GetPos());
                return;
            }

            if (const auto ironGolemMatch = IronGolemFull().Find(level, topPos)) {
                auto ironGolem = std::make_unique<IronGolem>(entities);
                ironGolem->SetPlayerCreated(true);
                // Spawns at the leg (getBlock(1, 2, 0)).
                SpawnGolemInWorld(level, *entities, *ironGolemMatch, std::move(ironGolem),
                                  ironGolemMatch->GetBlock(1, 2, 0).GetPos());
                return;
            }

            if (const auto copperGolemMatch = CopperGolemFull().Find(level, topPos)) {
                // Spawns where the head was (getBlock(0, 0, 0)); the copper
                // block becomes a copper chest.
                auto* copperGolem = static_cast<CopperGolem*>(
                    SpawnGolemInWorld(level, *entities, *copperGolemMatch, std::make_unique<CopperGolem>(entities),
                                      copperGolemMatch->GetBlock(0, 0, 0).GetPos()));
                ReplaceCopperBlockWithChest(level, *copperGolemMatch);
                // CopperGolem.spawn(getWeatherStateFromPattern(match)):
                // setWeatherState, then playSpawnSound.
                copperGolem->Spawn(WeatherStateFromPattern(*copperGolemMatch));
            }
        }

    } // namespace

    void CarvedPumpkinOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState,
                              BlockState oldState, bool /*movedByPiston*/) {
        // MC LevelChunk.setBlockState runs onPlace on the server only.
        if (level.IsClientSide()) return;
        // `if (!oldState.is(state.getBlock()))` — a facing-only rewrite of a
        // head (or a jack o'lantern over a carved pumpkin's own block) does
        // not re-check; any other block becoming a head does.
        if (oldState.Block() == newState.Block()) return;
        TrySpawnGolem(level, pos);
    }

    bool CarvedPumpkinCanSpawnGolem(const IBlockAccess& level, const glm::ivec3& topPos) {
        return SnowGolemBase().Find(level, topPos).has_value() ||
               IronGolemBase().Find(level, topPos).has_value() ||
               CopperGolemBase().Find(level, topPos).has_value();
    }

    void ClearPatternBlocks(ILevelWrite& level, const BlockPattern::Match& match) {
        for (int x = 0; x < match.GetWidth(); ++x) {
            for (int y = 0; y < match.GetHeight(); ++y) {
                BlockInWorld& block = match.GetBlock(x, y, 0);
                const glm::ivec3& pos = block.GetPos();
                // level.setBlock(pos, AIR, 2).
                level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::UpdateClients);
                // level.levelEvent(2001, pos, Block.getId(block.getState())) —
                // the state as matched, not the air just written.
                PlayLevelEventSound(level, nullptr, LevelEvent::PARTICLES_DESTROY_BLOCK, pos,
                                    static_cast<int>(MatchedState(block).RawId()), level.Random());
            }
        }
    }

    void UpdatePatternBlocks(ILevelWrite& level, const BlockPattern::Match& match) {
        for (int x = 0; x < match.GetWidth(); ++x) {
            for (int y = 0; y < match.GetHeight(); ++y) {
                level.UpdateNeighborsAt(match.GetBlock(x, y, 0).GetPos(), BlockID::Air);
            }
        }
    }

    void RegisterCarvedPumpkinBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        // Blocks.CARVED_PUMPKIN and Blocks.JACK_O_LANTERN are both
        // CarvedPumpkinBlocks.
        blocks[static_cast<size_t>(BlockID::CarvedPumpkin)].onPlace = &CarvedPumpkinOnPlace;
        blocks[static_cast<size_t>(BlockID::JackOLantern)].onPlace  = &CarvedPumpkinOnPlace;
    }

} // namespace Game
