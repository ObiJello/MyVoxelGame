// File: src/server/commands/FillCommand.cpp
#include "FillCommand.hpp"
#include "BlockCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../entity/ServerLevelBridge.hpp"
#include "../network/ServerConnection.hpp"

#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Server {

    namespace {

        // MC FillCommand.Mode: what happens to the cell before the block goes
        // in (Affector) and which block goes in (Filter).
        enum class Mode : uint8_t { Replace, Outline, Hollow, Destroy };

        bool OnShell(const glm::ivec3& p, const glm::ivec3& lo, const glm::ivec3& hi) {
            return p.x == lo.x || p.x == hi.x || p.y == lo.y || p.y == hi.y || p.z == lo.z || p.z == hi.z;
        }

        void Usage(ServerConnection& connection) {
            connection.SendChatMessage(
                "Usage: /fill <from> <to> <block> [destroy|hollow|keep|outline|replace [<filter>]|strict]", 1);
        }

    } // namespace

    void FillCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // wrapWithMode: the four mode literals after <block>, and again after
        // `replace <filter>`. `keep` and bare `replace` only follow <block>.
        const auto modes = [] { return Cmd::Literals({"destroy", "hollow", "outline", "strict"}); };
        dispatcher.RegisterCommand("fill", FillCommand::Execute,
            Cmd::Root().Then(Cmd::Argument("from", Cmd::Arg::BlockPos)
                .Then(Cmd::Argument("to", Cmd::Arg::BlockPos)
                    .Then(Cmd::Argument("block", Cmd::Arg::Block).Executes()
                        .Then(modes())
                        .Then(Cmd::Literal("keep").Executes())
                        .Then(Cmd::Literal("replace").Executes()
                            .Then(Cmd::Argument("filter", Cmd::Arg::BlockPredicate).Executes()
                                .Then(modes())))))));
    }

    void FillCommand::Execute(const CommandSourceStack& source,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& /*sessionManager*/) {
        if (args.size() < 7) { Usage(connection); return; }

        ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
        Game::World* world = level ? level->World() : nullptr;
        if (!world) { SendCommandFailure(connection, "No blocks were filled"); return; }

        // ── Arguments (MC parse order: from, to, block, then the suffix) ──
        std::string error;
        glm::ivec3 from, to;
        if (!GetLoadedBlockPos(*world, args[0], args[1], args[2], source, from, error) ||
            !GetLoadedBlockPos(*world, args[3], args[4], args[5], source, to, error)) {
            SendCommandFailure(connection, error);
            return;
        }
        BlockInput block;
        if (!ParseBlockInput(args[6], block, error)) { SendCommandFailure(connection, error); return; }

        Mode mode = Mode::Replace;
        bool strict = false;
        bool keep = false;
        std::optional<BlockFilter> filter;
        size_t at = 7;
        const auto readMode = [&](const std::string& word) {
            if (word == "outline")      mode = Mode::Outline;
            else if (word == "hollow")  mode = Mode::Hollow;
            else if (word == "destroy") mode = Mode::Destroy;
            else if (word == "strict")  strict = true;
            else return false;
            return true;
        };
        if (at < args.size()) {
            const std::string& word = args[at++];
            if (word == "keep") {
                keep = true;
            } else if (word == "replace") {
                if (at < args.size()) {
                    BlockFilter parsed;
                    if (!ParseBlockFilter(args[at++], parsed, error)) { SendCommandFailure(connection, error); return; }
                    filter = std::move(parsed);
                    if (at < args.size() && !readMode(args[at++])) { Usage(connection); return; }
                }
            } else if (!readMode(word)) {
                Usage(connection);
                return;
            }
        }
        if (at < args.size()) { Usage(connection); return; }

        // ── fillBlocks ─────────────────────────────────────────────────────
        const glm::ivec3 lo = glm::min(from, to);
        const glm::ivec3 hi = glm::max(from, to);
        const int64_t area = int64_t(hi.x - lo.x + 1) * int64_t(hi.y - lo.y + 1) * int64_t(hi.z - lo.z + 1);
        const int limit = Game::Rules::GetInt(Game::Rules::Id::MaxBlockModifications);
        if (area > limit) {
            SendCommandFailure(connection, "Too many blocks in the specified area (maximum " + std::to_string(limit) +
                                           ", specified " + std::to_string(area) + ")");
            return;
        }
        // MC's setBlock loads a chunk between the two corners on demand.
        if (!EnsureChunksLoaded(*level, lo, hi)) { SendCommandFailure(connection, "That position is not loaded"); return; }

        // 2 | 256, or 2 | 816 strict (MC Block.UPDATE_CLIENTS |
        // UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS, plus KNOWN_SHAPE,
        // SUPPRESS_DROPS and SKIP_ON_PLACE).
        using Flags = Game::World::UpdateFlags;
        const uint32_t flags = Flags::UpdateClients | Flags::SkipBlockEntitySideEffects |
            (strict ? (Flags::KnownShape | Flags::SuppressDrops | Flags::SkipOnPlace) : 0u);

        static const BlockInput kHollowCore = [] {
            BlockInput core;
            core.state = Game::BlockStates::Default(Game::BlockID::Air);
            return core;
        }();

        struct Updated { glm::ivec3 pos; Game::BlockState oldState; };
        std::vector<Updated> updated;
        int count = 0;
        // BlockPos.betweenClosed: x fastest, then y, then z.
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int y = lo.y; y <= hi.y; ++y) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    const glm::ivec3 pos(x, y, z);
                    if (filter && !filter->Test(*world, pos)) continue;
                    // `keep`: the predicate is Level.isEmptyBlock.
                    if (keep && world->GetBlockState(x, y, z).Block() != Game::BlockID::Air) continue;

                    const Game::BlockState oldState = world->GetBlockState(x, y, z);
                    bool affected = false;
                    if (mode == Mode::Destroy) {
                        // Level.destroyBlock(pos, true): false for air.
                        if (oldState.Block() != Game::BlockID::Air) {
                            if (level->MobLevel()) level->MobLevel()->DestroyBlock(pos, true);
                            else world->DestroyBlock(pos, true);
                            affected = true;
                        }
                    }

                    const BlockInput* input = &block;
                    if (mode == Mode::Outline && !OnShell(pos, lo, hi)) input = nullptr;
                    else if (mode == Mode::Hollow && !OnShell(pos, lo, hi)) input = &kHollowCore;

                    if (!input || !input->Place(*world, pos, flags)) {
                        if (affected) ++count;
                        continue;
                    }
                    if (!strict) updated.push_back({pos, oldState});
                    ++count;
                }
            }
        }
        for (const Updated& u : updated) UpdateNeighboursOnBlockSet(*world, u.pos, u.oldState);

        if (count == 0) { SendCommandFailure(connection, "No blocks were filled"); return; }
        source.SendSuccess(connection, "Successfully filled " + std::to_string(count) + " block(s)", /*broadcast=*/true);
    }

} // namespace Server
