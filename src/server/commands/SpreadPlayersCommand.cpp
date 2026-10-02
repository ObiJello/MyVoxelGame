// File: src/server/commands/SpreadPlayersCommand.cpp
#include "SpreadPlayersCommand.hpp"
#include "BlockCommandUtil.hpp"
#include "CommandCoords.hpp"
#include "EntitySelector.hpp"
#include "TeleportCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Server {

    namespace {

        constexpr int kMaxIterationCount = 10000;

        // The column reads below go through here: MC's getBlockState loads
        // a chunk on demand, this engine's World does not, so each column's
        // chunk is brought in (blocking, with a short ticket) the first time
        // it is read.
        struct ColumnReader {
            ServerLevel& level;
            Game::World& world;
            std::unordered_set<uint64_t> ensured;

            Game::BlockState Get(int x, int y, int z) {
                const int cx = x >> 4, cz = z >> 4;
                const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                                     static_cast<uint32_t>(cz);
                if (ensured.insert(key).second) level.GetChunkBlocking(Game::Math::ChunkPos(cx, cz));
                return world.GetBlockState(x, y, z);
            }
        };

        // MC SpreadPlayersCommand.Position.
        struct Position {
            double x = 0.0;
            double z = 0.0;

            double Dist(const Position& t) const {
                const double dx = x - t.x, dz = z - t.z;
                return std::sqrt(dx * dx + dz * dz);
            }
            double Length() const { return std::sqrt(x * x + z * z); }
            void Normalize() {
                const double d = Length();
                x /= d;
                z /= d;
            }
            void MoveAway(const Position& p) {
                x -= p.x;
                z -= p.z;
            }
            bool Clamp(double minX, double minZ, double maxX, double maxZ) {
                bool changed = false;
                if (x < minX) { x = minX; changed = true; }
                else if (x > maxX) { x = maxX; changed = true; }
                if (z < minZ) { z = minZ; changed = true; }
                else if (z > maxZ) { z = maxZ; changed = true; }
                return changed;
            }
            // The first block from maxHeight down with two air cells above it.
            int GetSpawnY(ColumnReader& level, int maxHeight) const {
                const int bx = static_cast<int>(std::floor(x));
                const int bz = static_cast<int>(std::floor(z));
                int y = maxHeight + 1;
                bool air2Above = level.Get(bx, y, bz).Block() == Game::BlockID::Air;
                --y;
                bool air1Above = level.Get(bx, y, bz).Block() == Game::BlockID::Air;
                while (y > Game::World::MIN_Y) {
                    --y;
                    const bool currentIsAir = level.Get(bx, y, bz).Block() == Game::BlockID::Air;
                    if (!currentIsAir && air1Above && air2Above) return y + 1;
                    air2Above = air1Above;
                    air1Above = currentIsAir;
                }
                return maxHeight + 1;
            }
            bool IsSafe(ColumnReader& level, int maxHeight) const {
                const int y = GetSpawnY(level, maxHeight) - 1;
                const Game::BlockState state =
                    level.Get(static_cast<int>(std::floor(x)), y, static_cast<int>(std::floor(z)));
                const Game::BlockID block = state.Block();
                const bool liquid = block == Game::BlockID::Water || block == Game::BlockID::Lava;
                return y < maxHeight && !liquid &&
                       Game::DataTags::HasTag(Game::DataTags::Registry::Block,
                                              Game::BlockRegistry::Get(block).registrySlug,
                                              "minecraft:entities_can_teleport_to");
            }
            // Mth.nextDouble(random, min, max).
            void Randomize(Game::JavaRandom& random, double minX, double minZ, double maxX, double maxZ) {
                x = minX >= maxX ? minX : random.NextDouble() * (maxX - minX) + minX;
                z = minZ >= maxZ ? minZ : random.NextDouble() * (maxZ - minZ) + minZ;
            }
        };

        std::string Format2(double v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            return buf;
        }

        bool ParseFloatArg(const std::string& text, float min, float& out, std::string& error) {
            char* end = nullptr;
            const float v = std::strtof(text.c_str(), &end);
            if (text.empty() || end == text.c_str() || *end != '\0') {
                error = "Invalid float '" + text + "'";
                return false;
            }
            if (v < min) {
                error = "Float must not be less than " + JavaFloatString(min) + ", found " + JavaFloatString(v);
                return false;
            }
            out = v;
            return true;
        }

        void Usage(ServerConnection& connection) {
            connection.SendChatMessage("Usage: /spreadplayers <center> <spreadDistance> <maxRange> "
                                       "[under <maxHeight>] <respectTeams> <targets>", 1);
        }

    } // namespace

    void SpreadPlayersCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        const auto teamsAndTargets = [] {
            return Cmd::Argument("respectTeams", Cmd::Arg::Bool)
                .Then(Cmd::Argument("targets", Cmd::Arg::Entities).Executes());
        };
        dispatcher.RegisterCommand("spreadplayers", SpreadPlayersCommand::Execute,
            Cmd::Root().Then(Cmd::Argument("center", Cmd::Arg::Vec2)
                .Then(Cmd::Argument("spreadDistance", Cmd::Arg::Float).Suggests({"0", "10"})
                    .Then(Cmd::Argument("maxRange", Cmd::Arg::Float).Suggests({"1", "100"})
                        .Then(teamsAndTargets())
                        .Then(Cmd::Literal("under")
                            .Then(Cmd::Argument("maxHeight", Cmd::Arg::Integer).Suggests({"64", "128"})
                                .Then(teamsAndTargets())))))));
    }

    void SpreadPlayersCommand::Execute(const CommandSourceStack& source,
                                       const std::vector<std::string>& args,
                                       ServerConnection& connection,
                                       PlayerSessionManager& /*sessionManager*/) {
        // center(2) spread max [under h] teams targets
        const bool under = args.size() == 8 && args[4] == "under";
        if (args.size() != 6 && !under) { Usage(connection); return; }

        ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
        Game::World* world = level ? level->World() : nullptr;
        if (!world) { SendCommandFailure(connection, "That position is not loaded"); return; }

        // Vec2Argument (centre-corrected, no local coordinates).
        std::string error;
        double cx = 0.0, cz = 0.0;
        if (args[0].rfind('^', 0) == 0 || args[1].rfind('^', 0) == 0) {
            SendCommandFailure(connection, "Cannot mix world & local coordinates (everything must either use ^ or not)");
            return;
        }
        if (!ParseCoord(args[0], source.position.x, true, cx) || !ParseCoord(args[1], source.position.z, true, cz)) {
            SendCommandFailure(connection, "Expected a coordinate");
            return;
        }
        const float centerX = static_cast<float>(cx);
        const float centerZ = static_cast<float>(cz);
        float spreadDistance = 0.0f, maxRange = 0.0f;
        if (!ParseFloatArg(args[2], 0.0f, spreadDistance, error) || !ParseFloatArg(args[3], 1.0f, maxRange, error)) {
            SendCommandFailure(connection, error);
            return;
        }
        // getMaxY() + 1 unless `under`.
        int maxHeight = Game::World::MAX_Y + 1;
        size_t at = 4;
        if (under) {
            char* end = nullptr;
            const long h = std::strtol(args[5].c_str(), &end, 10);
            if (args[5].empty() || *end != '\0') {
                SendCommandFailure(connection, "Invalid integer '" + args[5] + "'");
                return;
            }
            maxHeight = static_cast<int>(h);
            at = 6;
        }
        bool respectTeams = false;
        if (args[at] == "true") respectTeams = true;
        else if (args[at] != "false") {
            SendCommandFailure(connection, "Invalid boolean, expected 'true' or 'false' but found '" + args[at] + "'");
            return;
        }
        std::vector<SelectedEntity> entities;
        if (!ResolveSelector(args[at + 1], SelectorKind::Entities, source, entities, error)) {
            SendCommandFailure(connection, error);
            return;
        }

        // ── spreadPlayers ──────────────────────────────────────────────────
        const int minY = Game::World::MIN_Y;
        if (maxHeight < minY) {
            SendCommandFailure(connection, "Invalid maxHeight " + std::to_string(maxHeight) +
                                           "; expected higher than world minimum " + std::to_string(minY));
            return;
        }
        Game::JavaRandom random(static_cast<int64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        const double minX = static_cast<double>(centerX - maxRange);
        const double minZ = static_cast<double>(centerZ - maxRange);
        const double maxX = static_cast<double>(centerX + maxRange);
        const double maxZ = static_cast<double>(centerZ + maxRange);

        // getNumberOfTeams: every target is team-less here, one "team".
        const size_t count = respectTeams ? 1 : entities.size();
        std::vector<Position> positions(count);
        for (Position& p : positions) p.Randomize(random, minX, minZ, maxX, maxZ);

        ColumnReader reader{*level, *world, {}};

        // spreadPositions.
        bool hasCollisions = true;
        double minDistance = static_cast<double>(std::numeric_limits<float>::max());
        int iteration = 0;
        for (; iteration < kMaxIterationCount && hasCollisions; ++iteration) {
            hasCollisions = false;
            minDistance = static_cast<double>(std::numeric_limits<float>::max());
            for (size_t i = 0; i < positions.size(); ++i) {
                Position& position = positions[i];
                int neighbourCount = 0;
                Position average;
                for (size_t j = 0; j < positions.size(); ++j) {
                    if (i == j) continue;
                    const Position& neighbour = positions[j];
                    const double dist = position.Dist(neighbour);
                    minDistance = std::min(dist, minDistance);
                    if (dist < static_cast<double>(spreadDistance)) {
                        ++neighbourCount;
                        average.x += neighbour.x - position.x;
                        average.z += neighbour.z - position.z;
                    }
                }
                if (neighbourCount > 0) {
                    average.x /= neighbourCount;
                    average.z /= neighbourCount;
                    if (average.Length() > 0.0) {
                        average.Normalize();
                        position.MoveAway(average);
                    } else {
                        position.Randomize(random, minX, minZ, maxX, maxZ);
                    }
                    hasCollisions = true;
                }
                if (position.Clamp(minX, minZ, maxX, maxZ)) hasCollisions = true;
            }
            if (!hasCollisions) {
                for (Position& position : positions) {
                    if (!position.IsSafe(reader, maxHeight)) {
                        position.Randomize(random, minX, minZ, maxX, maxZ);
                        hasCollisions = true;
                    }
                }
            }
        }
        if (minDistance == static_cast<double>(std::numeric_limits<float>::max())) minDistance = 0.0;
        if (iteration >= kMaxIterationCount) {
            SendCommandFailure(connection,
                respectTeams
                    ? "Could not spread " + std::to_string(positions.size()) + " team(s) around " +
                          JavaFloatString(centerX) + ", " + JavaFloatString(centerZ) +
                          " (too many entities for space - try using spread of at most " + Format2(minDistance) + ")"
                    : "Could not spread " + std::to_string(positions.size()) + " entity/entities around " +
                          JavaFloatString(centerX) + ", " + JavaFloatString(centerZ) +
                          " (too many entities for space - try using spread of at most " + Format2(minDistance) + ")");
            return;
        }

        // setPlayerPositions.
        double avgDistance = 0.0;
        size_t positionIndex = 0;
        for (const SelectedEntity& entity : entities) {
            const Position& position = respectTeams ? positions[0] : positions[positionIndex++];
            const glm::dvec3 target(std::floor(position.x) + 0.5,
                                    static_cast<double>(position.GetSpawnY(reader, maxHeight)),
                                    std::floor(position.z) + 0.5);
            TeleportCommand::TeleportEntity(source, entity, source.dimension, target, entity.yRot, entity.xRot);
            double closest = std::numeric_limits<double>::max();
            for (const Position& test : positions) {
                if (&position != &test) closest = std::min(closest, position.Dist(test));
            }
            avgDistance += closest;
        }
        const double distance = entities.size() < 2 ? 0.0 : avgDistance / static_cast<double>(entities.size());

        source.SendSuccess(connection,
            std::string("Spread ") + std::to_string(positions.size()) +
            (respectTeams ? " team(s) around " : " entity/entities around ") +
            JavaFloatString(centerX) + ", " + JavaFloatString(centerZ) + " with an average distance of " +
            Format2(distance) + " block(s) apart", /*broadcast=*/true);
    }

} // namespace Server
