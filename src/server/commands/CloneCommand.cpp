// File: src/server/commands/CloneCommand.cpp
#include "CloneCommand.hpp"
#include "BlockCommandUtil.hpp"
#include "DimensionCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"

#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/ticks/ScheduledTick.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Server {

    namespace {

        using Compound = ::World::NBTTagCompound;

        // MC CloneCommands.Mode — whether the boxes may overlap.
        enum class Mode : uint8_t { Force, Move, Normal };

        // MC CloneCommands.CloneBlockInfo.
        struct CloneBlockInfo {
            glm::ivec3                pos;
            Game::BlockState          state;
            std::shared_ptr<Compound> blockEntity;    // saveCustomOnly; null = none
            Game::BlockState          previousStateAtDestination;
        };

        bool Intersects(const glm::ivec3& aLo, const glm::ivec3& aHi, const glm::ivec3& bLo, const glm::ivec3& bHi) {
            return aHi.x >= bLo.x && aLo.x <= bHi.x && aHi.y >= bLo.y && aLo.y <= bHi.y &&
                   aHi.z >= bLo.z && aLo.z <= bHi.z;
        }

        // Level.hasChunksAt(from, to).
        bool HasChunksAt(const Game::World& world, const glm::ivec3& lo, const glm::ivec3& hi) {
            for (int cx = lo.x >> 4; cx <= (hi.x >> 4); ++cx) {
                for (int cz = lo.z >> 4; cz <= (hi.z >> 4); ++cz) {
                    if (!world.IsChunkLoaded(cx, cz)) return false;
                }
            }
            return true;
        }

        // MC LevelTicks.copyAreaFrom(source, box, offset): every appointment
        // pending inside the box, moved by the offset, scheduled in the
        // destination with the delay it had left. The command runs between
        // ticks, so nothing is mid-drain (MC's toRunThisTick and
        // alreadyRunThisTick are empty here). Drain order is kept by
        // scheduling in it: the destination hands out fresh sub-tick numbers
        // in call order, which is what MC's re-based subTickOrder preserves.
        void CopyTicks(Game::World& from, Game::World& to, const glm::ivec3& lo, const glm::ivec3& hi,
                       const glm::ivec3& offset) {
            Game::ScheduledTickAccess* ticks = to.Ticks();
            if (!ticks) return;
            const int64_t gameTime = from.GetGameTime();
            std::vector<Game::SavedTick> found;
            for (int cx = lo.x >> 4; cx <= (hi.x >> 4); ++cx) {
                for (int cz = lo.z >> 4; cz <= (hi.z >> 4); ++cz) {
                    std::shared_ptr<Game::Chunk> chunk = from.GetLoadedChunk(cx, cz);
                    if (!chunk) continue;
                    std::vector<Game::SavedTick> packed;
                    {
                        const auto guard = chunk->LockShared();
                        packed = chunk->BlockTicks().Pack(gameTime);
                    }
                    for (const Game::SavedTick& t : packed) {
                        if (t.pos.x < lo.x || t.pos.x > hi.x || t.pos.y < lo.y || t.pos.y > hi.y ||
                            t.pos.z < lo.z || t.pos.z > hi.z) continue;
                        found.push_back(t);
                    }
                }
            }
            std::stable_sort(found.begin(), found.end(), [](const Game::SavedTick& a, const Game::SavedTick& b) {
                if (a.delay != b.delay) return a.delay < b.delay;
                return a.priority < b.priority;
            });
            for (const Game::SavedTick& t : found) {
                ticks->ScheduleTick(t.pos + offset, t.type, t.delay, t.priority);
            }
        }

        void Usage(ServerConnection& connection) {
            connection.SendChatMessage(
                "Usage: /clone [from <sourceDimension>] <begin> <end> [to <targetDimension>] <destination> "
                "[strict] [replace|masked|filtered <filter>] [force|move|normal]", 1);
        }

    } // namespace

    void CloneCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // modeSuffix: executable, then replace / masked / filtered <filter>,
        // each with an optional force|move|normal (wrapWithCloneMode).
        const auto suffix = [](Cmd::Node node) {
            const auto modes = [] { return Cmd::Literals({"force", "move", "normal"}); };
            node.Executes()
                .Then(Cmd::Literal("replace").Executes().Then(modes()))
                .Then(Cmd::Literal("masked").Executes().Then(modes()))
                .Then(Cmd::Literal("filtered")
                    .Then(Cmd::Argument("filter", Cmd::Arg::BlockPredicate).Executes().Then(modes())));
            return node;
        };
        // destinationAndStrictSuffix.
        Cmd::Node destination = suffix(Cmd::Argument("destination", Cmd::Arg::BlockPos));
        destination.Then(suffix(Cmd::Literal("strict")));
        // beginEndDestinationAndModeSuffix.
        Cmd::Node begin = Cmd::Argument("begin", Cmd::Arg::BlockPos)
            .Then(Cmd::Argument("end", Cmd::Arg::BlockPos)
                .Then(destination)
                .Then(Cmd::Literal("to")
                    .Then(Cmd::Argument("targetDimension", Cmd::Arg::Dimension).Then(destination))));
        dispatcher.RegisterCommand("clone", CloneCommand::Execute,
            Cmd::Root()
                .Then(begin)
                .Then(Cmd::Literal("from").Then(Cmd::Argument("sourceDimension", Cmd::Arg::Dimension).Then(begin))));
    }

    void CloneCommand::Execute(const CommandSourceStack& source,
                               const std::vector<std::string>& args,
                               ServerConnection& connection,
                               PlayerSessionManager& /*sessionManager*/) {
        if (!g_integratedServer) { SendCommandFailure(connection, "No blocks were cloned"); return; }

        // ── Arguments ──────────────────────────────────────────────────────
        size_t at = 0;
        const auto has = [&](size_t n) { return at + n <= args.size(); };
        Game::DimensionId fromDim = source.dimension;
        Game::DimensionId toDim = source.dimension;
        if (has(1) && args[at] == "from") {
            if (!has(2)) { Usage(connection); return; }
            const auto dim = DimensionCommand::ParseDimension(args[at + 1]);
            if (!dim) { SendCommandFailure(connection, "Unknown dimension '" + args[at + 1] + "'"); return; }
            fromDim = *dim;
            at += 2;
        }
        if (!has(6)) { Usage(connection); return; }
        const size_t beginAt = at, endAt = at + 3;
        at += 6;
        if (has(1) && args[at] == "to") {
            if (!has(2)) { Usage(connection); return; }
            const auto dim = DimensionCommand::ParseDimension(args[at + 1]);
            if (!dim) { SendCommandFailure(connection, "Unknown dimension '" + args[at + 1] + "'"); return; }
            toDim = *dim;
            at += 2;
        }
        if (!has(3)) { Usage(connection); return; }
        const size_t destAt = at;
        at += 3;
        bool strict = false;
        if (has(1) && args[at] == "strict") { strict = true; ++at; }
        bool masked = false;
        std::optional<BlockFilter> filter;
        Mode mode = Mode::Normal;
        std::string error;
        if (has(1)) {
            const std::string& word = args[at++];
            if (word == "masked") {
                masked = true;
            } else if (word == "filtered") {
                if (!has(1)) { Usage(connection); return; }
                BlockFilter parsed;
                if (!ParseBlockFilter(args[at++], parsed, error)) { SendCommandFailure(connection, error); return; }
                filter = std::move(parsed);
            } else if (word != "replace") {
                Usage(connection);
                return;
            }
            if (has(1)) {
                const std::string& m = args[at++];
                if (m == "force") mode = Mode::Force;
                else if (m == "move") mode = Mode::Move;
                else if (m == "normal") mode = Mode::Normal;
                else { Usage(connection); return; }
            }
        }
        if (at != args.size()) { Usage(connection); return; }

        ServerLevel* fromLevel = g_integratedServer->GetOrCreateLevel(fromDim);
        ServerLevel* toLevel = g_integratedServer->GetOrCreateLevel(toDim);
        Game::World* fromWorld = fromLevel ? fromLevel->World() : nullptr;
        Game::World* toWorld = toLevel ? toLevel->World() : nullptr;
        if (!fromWorld || !toWorld) { SendCommandFailure(connection, "That position is not loaded"); return; }

        glm::ivec3 begin, end, dest;
        if (!GetLoadedBlockPos(*fromWorld, args[beginAt], args[beginAt + 1], args[beginAt + 2], source, begin, error) ||
            !GetLoadedBlockPos(*fromWorld, args[endAt], args[endAt + 1], args[endAt + 2], source, end, error) ||
            !GetLoadedBlockPos(*toWorld, args[destAt], args[destAt + 1], args[destAt + 2], source, dest, error)) {
            SendCommandFailure(connection, error);
            return;
        }

        // ── clone ──────────────────────────────────────────────────────────
        const glm::ivec3 fromLo = glm::min(begin, end);
        const glm::ivec3 fromHi = glm::max(begin, end);
        const glm::ivec3 destLo = glm::min(dest, dest + (fromHi - fromLo));
        const glm::ivec3 destHi = glm::max(dest, dest + (fromHi - fromLo));

        if (mode == Mode::Normal && fromWorld == toWorld && Intersects(destLo, destHi, fromLo, fromHi)) {
            SendCommandFailure(connection, "The source and destination areas cannot overlap");
            return;
        }
        const int64_t area = int64_t(fromHi.x - fromLo.x + 1) * int64_t(fromHi.y - fromLo.y + 1) *
                             int64_t(fromHi.z - fromLo.z + 1);
        const int limit = Game::Rules::GetInt(Game::Rules::Id::MaxBlockModifications);
        if (area > limit) {
            SendCommandFailure(connection, "Too many blocks in the specified area (maximum " + std::to_string(limit) +
                                           ", specified " + std::to_string(area) + ")");
            return;
        }
        if (!HasChunksAt(*fromWorld, fromLo, fromHi) || !HasChunksAt(*toWorld, destLo, destHi)) {
            SendCommandFailure(connection, "That position is not loaded");
            return;
        }

        std::vector<CloneBlockInfo> solidList, blockEntitiesList, otherBlocksList;
        std::deque<glm::ivec3> clearBlocksList;
        const glm::ivec3 offset = destLo - fromLo;

        // Source order: z, then y, then x fastest.
        for (int z = fromLo.z; z <= fromHi.z; ++z) {
            for (int y = fromLo.y; y <= fromHi.y; ++y) {
                for (int x = fromLo.x; x <= fromHi.x; ++x) {
                    const glm::ivec3 sourcePos(x, y, z);
                    const glm::ivec3 destinationPos = sourcePos + offset;
                    const Game::BlockState state = fromWorld->GetBlockState(x, y, z);
                    if (masked && state.Block() == Game::BlockID::Air) continue;   // FILTER_AIR
                    if (filter && !filter->Test(*fromWorld, sourcePos)) continue;
                    const Game::BlockState previous =
                        toWorld->GetBlockState(destinationPos.x, destinationPos.y, destinationPos.z);
                    if (fromWorld->GetBlockEntity(sourcePos)) {
                        // blockEntity.saveCustomOnly: the fields without id
                        // and position.
                        std::shared_ptr<Compound> tag = SaveBlockEntityTag(*fromWorld, sourcePos);
                        if (tag) for (const char* key : {"id", "x", "y", "z"}) tag->value.erase(key);
                        blockEntitiesList.push_back({destinationPos, state, std::move(tag), previous});
                        clearBlocksList.push_back(sourcePos);
                    } else if (!Game::Lighting::BlockLightProperties::SolidRender(state) &&
                               !Game::Lighting::BlockLightProperties::FullCollision(state)) {
                        otherBlocksList.push_back({destinationPos, state, nullptr, previous});
                        clearBlocksList.push_front(sourcePos);
                    } else {
                        solidList.push_back({destinationPos, state, nullptr, previous});
                        clearBlocksList.push_back(sourcePos);
                    }
                }
            }
        }

        using Flags = Game::World::UpdateFlags;
        const uint32_t kStrictFlags = Flags::KnownShape | Flags::SuppressDrops |
                                      Flags::SkipBlockEntitySideEffects | Flags::SkipOnPlace;   // 816
        const uint32_t defaultUpdateFlags = Flags::UpdateClients | (strict ? kStrictFlags : 0u);
        const Game::BlockState barrier = Game::BlockStates::Default(Game::BlockID::Barrier);
        const Game::BlockState air = Game::BlockStates::Default(Game::BlockID::Air);

        if (mode == Mode::Move) {
            // Barrier first (no drops, no side effects), then air with the
            // real updates — a chest moved out does not spill.
            for (const glm::ivec3& pos : clearBlocksList) {
                fromWorld->SetBlock(pos, barrier, defaultUpdateFlags | kStrictFlags, Game::World::kUpdateLimit);
            }
            const uint32_t standardUpdateFlags = strict ? defaultUpdateFlags : static_cast<uint32_t>(Flags::All);
            for (const glm::ivec3& pos : clearBlocksList) {
                fromWorld->SetBlock(pos, air, standardUpdateFlags, Game::World::kUpdateLimit);
            }
        }

        std::vector<CloneBlockInfo> blockInfoList;
        blockInfoList.reserve(solidList.size() + blockEntitiesList.size() + otherBlocksList.size());
        blockInfoList.insert(blockInfoList.end(), solidList.begin(), solidList.end());
        blockInfoList.insert(blockInfoList.end(), blockEntitiesList.begin(), blockEntitiesList.end());
        blockInfoList.insert(blockInfoList.end(), otherBlocksList.begin(), otherBlocksList.end());

        for (auto it = blockInfoList.rbegin(); it != blockInfoList.rend(); ++it) {
            toWorld->SetBlock(it->pos, barrier, defaultUpdateFlags | kStrictFlags, Game::World::kUpdateLimit);
        }
        int count = 0;
        for (const CloneBlockInfo& info : blockInfoList) {
            if (toWorld->SetBlock(info.pos, info.state, defaultUpdateFlags, Game::World::kUpdateLimit)) ++count;
        }
        for (const CloneBlockInfo& info : blockEntitiesList) {
            // loadCustomOnly into the entity the placement made, then the
            // state set again (MC's loop step).
            if (info.blockEntity && toWorld->GetBlockEntity(info.pos)) {
                LoadBlockEntityTag(*toWorld, info.pos, *info.blockEntity);
            }
            toWorld->SetBlock(info.pos, info.state, defaultUpdateFlags, Game::World::kUpdateLimit);
        }
        if (!strict) {
            for (auto it = blockInfoList.rbegin(); it != blockInfoList.rend(); ++it) {
                UpdateNeighboursOnBlockSet(*toWorld, it->pos, it->previousStateAtDestination);
            }
        }
        CopyTicks(*fromWorld, *toWorld, fromLo, fromHi, offset);

        if (count == 0) { SendCommandFailure(connection, "No blocks were cloned"); return; }
        source.SendSuccess(connection, "Successfully cloned " + std::to_string(count) + " block(s)", /*broadcast=*/true);
    }

} // namespace Server
