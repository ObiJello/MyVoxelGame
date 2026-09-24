// File: src/server/commands/ExecuteCommand.cpp
#include "ExecuteCommand.hpp"
#include "common/world/level/GameRules.hpp"
#include "BlockStateArgument.hpp"
#include "CommandCoords.hpp"
#include "DimensionCommand.hpp"
#include "EntitySelector.hpp"
#include "SummonCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/MobManager.hpp"
#include "../entity/ServerLevelBridge.hpp"   // PlayerEntityView
#include "../level/LocateFinder.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Server {

    void ExecuteCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("execute", ExecuteCommand::Execute);
    }

    namespace {

        using Sources = std::vector<CommandSourceStack>;

        constexpr const char* kUsage =
            "Usage: /execute (run <command> | as <targets> | at <targets> | "
            "positioned (<x> <y> <z> | as <targets> | over <heightmap>) | "
            "rotated (<yaw> <pitch> | as <targets>) | "
            "facing (<x> <y> <z> | entity <targets> <feet|eyes>) | "
            "align <axes> | anchored <feet|eyes> | in <dimension> | summon <entity> | "
            "on <attacker|controller|leasher|origin|owner|passengers|target|vehicle> | "
            "(if|unless) (block <pos> <block> | blocks <start> <end> <destination> (all|masked) | "
            "biome <pos> <biome> | dimension <dimension> | entity <targets> | loaded <pos>)) ...";

        // MC ExecuteCommand.MAX_TEST_AREA.
        constexpr int64_t kMaxTestArea = 32768;

        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Everything the subcommand parsers share.
        struct Chain {
            const std::vector<std::string>& args;
            ServerConnection&               connection;
            PlayerSessionManager&           sessions;
            size_t                          i = 0;      // next token

            bool   Has(size_t n) const { return i + n <= args.size(); }
            const std::string& Next() { return args[i++]; }

            void Fail(const std::string& message) const { connection.SendChatMessage(message, 1); }
            // MC Commands.performCommand / ExecutionContext fork limit — the
            // max_command_forks rule: "Maximum number of contexts (%s) reached".
            bool WithinForkLimit(size_t forks) const {
                const int limit = Game::Rules::GetInt(Game::Rules::Id::MaxCommandForks);
                if (forks <= static_cast<size_t>(limit)) return true;
                Fail("Maximum number of contexts (" + std::to_string(limit) + ") reached");
                return false;
            }
            void Incomplete() const {
                // MC: "Unknown or incomplete command, see below for error".
                Fail("Unknown or incomplete command, see below for error");
                Fail(kUsage);
            }
        };

        // EntityArgument.getOptionalEntities against one source — zero
        // matches is an empty fork, not an error.
        bool OptionalEntities(Chain& c, const std::string& token, const CommandSourceStack& source,
                              std::vector<SelectedEntity>& out) {
            std::string error;
            if (!ResolveSelector(token, SelectorKind::OptionalEntities, source, out, error)) {
                c.Fail(error);
                return false;
            }
            return true;
        }

        ServerLevel* LevelOf(const CommandSourceStack& source) {
            return g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
        }

        // BlockPosArgument.getLoadedBlockPos: parse, then require the chunk.
        bool LoadedBlockPos(Chain& c, const CommandSourceStack& source, size_t at,
                            glm::ivec3& out, Game::World*& world) {
            std::string error;
            if (!ParseBlockPos(c.args[at], c.args[at + 1], c.args[at + 2], source, source.rotation, out, error)) {
                c.Fail(error);
                return false;
            }
            ServerLevel* level = LevelOf(source);
            world = level ? level->World() : nullptr;
            if (!world || !world->IsChunkLoaded(out.x >> 4, out.z >> 4)) {
                c.Fail("That position is not loaded");
                return false;
            }
            if (!world->IsValidPosition(out.x, out.y, out.z)) {
                c.Fail("That position is out of this world!");
                return false;
            }
            return true;
        }

        std::optional<Game::HeightmapType> ParseHeightmap(const std::string& text) {
            const std::string t = Lower(text);
            if (t == "world_surface")             return Game::HeightmapType::WorldSurface;
            if (t == "motion_blocking")           return Game::HeightmapType::MotionBlocking;
            if (t == "motion_blocking_no_leaves") return Game::HeightmapType::MotionBlockingNoLeaves;
            if (t == "ocean_floor")               return Game::HeightmapType::OceanFloor;
            return std::nullopt;
        }

        // EntityAnchorArgument: "feet" / "eyes".
        bool ParseAnchor(Chain& c, const std::string& text, bool& eyes) {
            const std::string t = Lower(text);
            if (t == "feet") { eyes = false; return true; }
            if (t == "eyes") { eyes = true;  return true; }
            c.Fail("Invalid entity anchor position " + text);
            return false;
        }

        // RotationArgument: `<yaw> <pitch>`, `~` relative to the source's
        // rotation, no `^`.
        bool ParseRotation(Chain& c, const CommandSourceStack& source, size_t at, CommandRotation& out) {
            const std::string& ay = c.args[at];
            const std::string& ax = c.args[at + 1];
            if ((!ay.empty() && ay[0] == '^') || (!ax.empty() && ax[0] == '^')) {
                c.Fail("Local coordinates are not allowed here");
                return false;
            }
            double y = 0.0, x = 0.0;
            if (!ParseCoord(ay, source.rotation.yRot, false, y) ||
                !ParseCoord(ax, source.rotation.xRot, false, x)) {
                c.Fail("Invalid rotation: " + ay + " " + ax);
                return false;
            }
            out.yRot = static_cast<float>(y);
            out.xRot = static_cast<float>(x);
            return true;
        }

        // The live Entity behind a source's entity, for the relations. A
        // dropped item is not an Entity here and has no relations.
        Game::Entity* EntityOf(const CommandSourceStack& source) {
            if (!source.entity || !g_integratedServer) return nullptr;
            switch (source.entity->kind) {
                case SelectedEntity::Kind::Player:
                    return g_integratedServer->GetPlayerEntityView(static_cast<uint32_t>(source.entity->id));
                case SelectedEntity::Kind::Mob:
                    return source.entity->mob;
                case SelectedEntity::Kind::Item:
                    return nullptr;
            }
            return nullptr;
        }

        // ── Relations (`/execute on`, MC createRelationOperations) ──────────

        bool ForkRelation(Chain& c, const std::string& relation, Sources& sources) {
            Sources next;
            for (const CommandSourceStack& source : sources) {
                Game::Entity* self = EntityOf(source);
                if (!self) continue;
                std::vector<Game::Entity*> related;
                if (relation == "owner") {
                    // MC OwnableEntity — tamed animals.
                    if (auto* tamable = dynamic_cast<Game::TamableAnimal*>(self)) related.push_back(tamable->GetOwner());
                } else if (relation == "leasher") {
                    // No leads in this engine: nothing is Leashable.
                } else if (relation == "target") {
                    if (auto* mob = dynamic_cast<Game::Mob*>(self)) related.push_back(mob->GetTarget());
                } else if (relation == "attacker") {
                    if (auto* living = dynamic_cast<Game::LivingEntity*>(self)) related.push_back(living->GetLastHurtByMob());
                } else if (relation == "vehicle") {
                    related.push_back(self->GetVehicle());
                } else if (relation == "controller") {
                    related.push_back(self->GetControllingPassenger());
                } else if (relation == "origin") {
                    // MC TraceableEntity — a projectile's shooter.
                    if (auto* projectile = dynamic_cast<Game::Projectile*>(self)) related.push_back(projectile->GetOwner());
                } else if (relation == "passengers") {
                    for (Game::Entity* p : self->GetPassengers()) related.push_back(p);
                } else {
                    c.Fail("Unknown relation '" + relation + "' (attacker, controller, leasher, origin, owner, passengers, target, vehicle)");
                    return false;
                }
                for (Game::Entity* e : related) {
                    SelectedEntity described;
                    // MC filters removed entities; DescribeEntity does too.
                    if (e && DescribeEntity(e, source, described)) next.push_back(source.WithEntity(described));
                }
            }
            sources = std::move(next);
            return c.WithinForkLimit(sources.size());
        }

        // ── Conditionals (`if` / `unless`, MC addConditionals) ──────────────

        // One source's verdict. `counted` says the test has a count MC
        // reports ("Test passed. Count: n") — entity and blocks.
        struct Verdict { bool pass = false; int count = 0; bool counted = false; };

        // Returns false on an ERROR (already reported). `consumed` is how many
        // tokens after the `if`/`unless` word the test used.
        bool TestCondition(Chain& c, const CommandSourceStack& source, size_t& consumed, Verdict& v) {
            const size_t at = c.i;   // the word after if/unless
            if (!c.Has(1)) { c.Incomplete(); return false; }
            const std::string word = Lower(c.args[at]);

            if (word == "block") {
                if (!c.Has(5)) { c.Incomplete(); return false; }
                glm::ivec3 pos; Game::World* world = nullptr;
                if (!LoadedBlockPos(c, source, at + 1, pos, world)) return false;
                BlockPredicate pred; std::string error;
                if (!ParseBlockPredicate(c.args[at + 4], pred, error)) { c.Fail(error); return false; }
                v.pass = pred.Test(world->GetBlockState(pos.x, pos.y, pos.z));
                consumed = 5;
                return true;
            }
            if (word == "blocks") {
                if (!c.Has(11)) { c.Incomplete(); return false; }
                glm::ivec3 start, end, dest; Game::World* world = nullptr;
                if (!LoadedBlockPos(c, source, at + 1, start, world) ||
                    !LoadedBlockPos(c, source, at + 4, end, world) ||
                    !LoadedBlockPos(c, source, at + 7, dest, world)) return false;
                const std::string mode = Lower(c.args[at + 10]);
                if (mode != "all" && mode != "masked") {
                    c.Fail("Expected 'all' or 'masked', got '" + c.args[at + 10] + "'");
                    return false;
                }
                const bool skipAir = mode == "masked";
                // MC checkRegions: BoundingBox.fromCorners, the destination
                // box the same size, every source block compared to its
                // offset twin. Air is skipped in masked mode.
                const glm::ivec3 lo = glm::min(start, end), hi = glm::max(start, end);
                const glm::ivec3 size = hi - lo + glm::ivec3(1);
                const int64_t area = int64_t(size.x) * int64_t(size.y) * int64_t(size.z);
                if (area > kMaxTestArea) {
                    c.Fail("Too many blocks in the specified area (maximum " + std::to_string(kMaxTestArea) +
                           ", but specified " + std::to_string(area) + ")");
                    return false;
                }
                const glm::ivec3 offset = dest - lo;
                int count = 0;
                bool same = true;
                for (int z = lo.z; z <= hi.z && same; ++z)
                    for (int y = lo.y; y <= hi.y && same; ++y)
                        for (int x = lo.x; x <= hi.x; ++x) {
                            const Game::BlockState a = world->GetBlockState(x, y, z);
                            if (skipAir && a.Block() == Game::BlockID::Air) continue;
                            const glm::ivec3 d = glm::ivec3(x, y, z) + offset;
                            if (!world->IsChunkLoaded(d.x >> 4, d.z >> 4)) {
                                c.Fail("That position is not loaded");
                                return false;
                            }
                            if (a != world->GetBlockState(d.x, d.y, d.z)) { same = false; break; }
                            ++count;
                        }
                v.pass = same;
                v.count = same ? count : 0;
                v.counted = true;
                consumed = 11;
                return true;
            }
            if (word == "biome") {
                if (!c.Has(5)) { c.Incomplete(); return false; }
                glm::ivec3 pos; Game::World* world = nullptr;
                if (!LoadedBlockPos(c, source, at + 1, pos, world)) return false;
                const std::string& asked = c.args[at + 4];
                // ResourceOrTagArgument: a biome id or a #tag of biomes.
                const bool isTag = !asked.empty() && asked[0] == '#';
                const auto ids = ResolveBiomeIdOrTag(asked);
                if (ids.empty()) {
                    c.Fail(isTag ? "Can't find tag '" + asked.substr(1) + "' of type 'minecraft:worldgen/biome'"
                                 : "Can't find element '" + asked + "' of type 'minecraft:worldgen/biome'");
                    return false;
                }
                ServerLevel* level = LevelOf(source);
                const std::string here = level ? BiomeAt(*level, pos) : std::string();
                v.pass = !here.empty() && ids.count(here) != 0;
                consumed = 5;
                return true;
            }
            if (word == "loaded") {
                if (!c.Has(4)) { c.Incomplete(); return false; }
                glm::ivec3 pos; std::string error;
                if (!ParseBlockPos(c.args[at + 1], c.args[at + 2], c.args[at + 3], source, source.rotation, pos, error)) {
                    c.Fail(error);
                    return false;
                }
                ServerLevel* level = LevelOf(source);
                Game::World* world = level ? level->World() : nullptr;
                // MC isChunkLoaded wants ENTITY_TICKING; a loaded chunk is the
                // closest this engine's chunk states come.
                v.pass = world && world->IsChunkLoaded(pos.x >> 4, pos.z >> 4);
                consumed = 4;
                return true;
            }
            if (word == "dimension") {
                if (!c.Has(2)) { c.Incomplete(); return false; }
                const auto dim = DimensionCommand::ParseDimension(c.args[at + 1]);
                if (!dim) { c.Fail("Unknown dimension '" + c.args[at + 1] + "'"); return false; }
                v.pass = *dim == source.dimension;
                consumed = 2;
                return true;
            }
            if (word == "entity") {
                if (!c.Has(2)) { c.Incomplete(); return false; }
                std::vector<SelectedEntity> found;
                if (!OptionalEntities(c, c.args[at + 1], source, found)) return false;
                v.count = static_cast<int>(found.size());
                v.pass = v.count > 0;
                v.counted = true;
                consumed = 2;
                return true;
            }
            if (word == "score" || word == "predicate" || word == "function" || word == "data" ||
                word == "items" || word == "slots" || word == "stopwatch") {
                c.Fail("'if " + word + "' is not available in this game (no scoreboard, NBT, item slots, "
                       "loot predicates, functions or stopwatches)");
                return false;
            }
            c.Incomplete();
            return false;
        }

        // `if`/`unless` over every source. Non-terminal: keep the sources
        // whose verdict matches `expected` (MC expect()). Terminal (nothing
        // after the test): report per source, vanilla's messages.
        bool Conditional(Chain& c, bool expected, Sources& sources) {
            size_t consumed = 0;
            Sources kept;
            std::vector<Verdict> verdicts;
            for (const CommandSourceStack& source : sources) {
                Verdict v;
                if (!TestCondition(c, source, consumed, v)) return false;
                verdicts.push_back(v);
                if (v.pass == expected) kept.push_back(source);
            }
            if (sources.empty()) {
                // Every fork is already empty, so there is no source to
                // evaluate the test against; its tokens are skipped by shape
                // so the rest of the chain still parses. Nothing runs either
                // way.
                const std::string word = c.Has(1) ? Lower(c.args[c.i]) : "";
                if      (word == "block")     consumed = 5;
                else if (word == "blocks")    consumed = 11;
                else if (word == "biome")     consumed = 5;
                else if (word == "loaded")    consumed = 4;
                else if (word == "dimension") consumed = 2;
                else if (word == "entity")    consumed = 2;
                else { c.Incomplete(); return false; }
                if (!c.Has(consumed)) { c.Incomplete(); return false; }
            }
            c.i += consumed;

            const bool terminal = c.i >= c.args.size();
            if (terminal) {
                // MC addConditional.executes / createNumericConditionalHandler
                // / checkIfRegions: one message per source.
                for (const Verdict& v : verdicts) {
                    if (expected) {
                        if (v.pass) c.connection.SendChatMessage(v.counted ? "Test passed. Count: " + std::to_string(v.count) : "Test passed", 1);
                        else        c.connection.SendChatMessage("Test failed", 1);
                    } else {
                        if (!v.pass) c.connection.SendChatMessage("Test passed", 1);
                        else         c.connection.SendChatMessage(v.counted ? "Test failed. Count: " + std::to_string(v.count) : "Test failed", 1);
                    }
                }
                sources.clear();
                return true;
            }
            sources = std::move(kept);
            return true;
        }

    } // namespace

    void ExecuteCommand::Execute(const CommandSourceStack& initial,
                                 const std::vector<std::string>& args,
                                 ServerConnection& connection,
                                 PlayerSessionManager& sessionManager) {
        Chain c{ args, connection, sessionManager, 0 };
        Sources sources{ initial };

        if (args.empty()) { c.Incomplete(); return; }

        while (c.Has(1)) {
            const std::string word = Lower(c.Next());

            // ── run <command> ───────────────────────────────────────────────
            if (word == "run") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                std::string rest;
                for (size_t k = c.i; k < args.size(); ++k) {
                    if (!rest.empty()) rest += ' ';
                    rest += args[k];
                }
                if (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
                if (!g_integratedServer) return;
                CommandDispatcher& dispatcher = g_integratedServer->GetCommandDispatcher();
                // Once per fork. An empty fork runs nothing and says nothing,
                // as vanilla.
                for (const CommandSourceStack& source : sources) {
                    dispatcher.ExecuteCommand(rest, source, connection, sessionManager);
                }
                return;
            }

            // ── as / at <targets> ───────────────────────────────────────────
            if (word == "as" || word == "at") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                const std::string& selector = c.Next();
                Sources next;
                for (const CommandSourceStack& source : sources) {
                    std::vector<SelectedEntity> found;
                    if (!OptionalEntities(c, selector, source, found)) return;
                    for (const SelectedEntity& e : found) {
                        if (word == "as") {
                            next.push_back(source.WithEntity(e));
                        } else {
                            // MC: withLevel(entity.level()).withPosition(entity.position()).withRotation(entity.getRotationVector())
                            next.push_back(source.WithLevel(e.dimension)
                                                 .WithPosition(e.position)
                                                 .WithRotation(CommandRotation{ e.yRot, e.xRot }));
                        }
                    }
                }
                sources = std::move(next);
                if (!c.WithinForkLimit(sources.size())) return;
                continue;
            }

            // ── positioned ──────────────────────────────────────────────────
            if (word == "positioned") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                if (Lower(args[c.i]) == "as") {
                    if (!c.Has(2)) { c.Incomplete(); return; }
                    const std::string& selector = args[c.i + 1];
                    c.i += 2;
                    Sources next;
                    for (const CommandSourceStack& source : sources) {
                        std::vector<SelectedEntity> found;
                        if (!OptionalEntities(c, selector, source, found)) return;
                        for (const SelectedEntity& e : found) next.push_back(source.WithPosition(e.position));
                    }
                    sources = std::move(next);
                    if (!c.WithinForkLimit(sources.size())) return;
                    continue;
                }
                if (Lower(args[c.i]) == "over") {
                    if (!c.Has(2)) { c.Incomplete(); return; }
                    const auto type = ParseHeightmap(args[c.i + 1]);
                    if (!type) { c.Fail("Unknown heightmap '" + args[c.i + 1] + "' (world_surface, motion_blocking, motion_blocking_no_leaves, ocean_floor)"); return; }
                    c.i += 2;
                    Sources next;
                    for (const CommandSourceStack& source : sources) {
                        ServerLevel* level = LevelOf(source);
                        Game::World* world = level ? level->World() : nullptr;
                        const int bx = static_cast<int>(std::floor(source.position.x));
                        const int bz = static_cast<int>(std::floor(source.position.z));
                        if (!world || !world->IsChunkLoaded(bx >> 4, bz >> 4)) { c.Fail("That position is not loaded"); return; }
                        // MC Level.getHeight: the heightmap value, which is one
                        // ABOVE the topmost counted block; this engine's
                        // surface height is that block's own Y.
                        const int height = world->GetSurfaceHeight(bx, bz, *type) + 1;
                        next.push_back(source.WithPosition(glm::dvec3(source.position.x, height, source.position.z)));
                    }
                    sources = std::move(next);
                    if (!c.WithinForkLimit(sources.size())) return;
                    continue;
                }
                if (!c.Has(3)) { c.Incomplete(); return; }
                Sources next;
                for (const CommandSourceStack& source : sources) {
                    glm::dvec3 pos; std::string error;
                    if (!ParseVec3(args[c.i], args[c.i + 1], args[c.i + 2], source, source.rotation, pos, error)) { c.Fail(error); return; }
                    // MC: withPosition(pos).withAnchor(FEET)
                    next.push_back(source.WithPosition(pos).WithAnchor(false));
                }
                c.i += 3;
                sources = std::move(next);
                if (!c.WithinForkLimit(sources.size())) return;
                continue;
            }

            // ── rotated ─────────────────────────────────────────────────────
            if (word == "rotated") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                if (Lower(args[c.i]) == "as") {
                    if (!c.Has(2)) { c.Incomplete(); return; }
                    const std::string& selector = args[c.i + 1];
                    c.i += 2;
                    Sources next;
                    for (const CommandSourceStack& source : sources) {
                        std::vector<SelectedEntity> found;
                        if (!OptionalEntities(c, selector, source, found)) return;
                        for (const SelectedEntity& e : found) next.push_back(source.WithRotation(CommandRotation{ e.yRot, e.xRot }));
                    }
                    sources = std::move(next);
                    if (!c.WithinForkLimit(sources.size())) return;
                    continue;
                }
                if (!c.Has(2)) { c.Incomplete(); return; }
                Sources next;
                for (const CommandSourceStack& source : sources) {
                    CommandRotation rot;
                    if (!ParseRotation(c, source, c.i, rot)) return;
                    next.push_back(source.WithRotation(rot));
                }
                c.i += 2;
                sources = std::move(next);
                if (!c.WithinForkLimit(sources.size())) return;
                continue;
            }

            // ── facing ──────────────────────────────────────────────────────
            if (word == "facing") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                if (Lower(args[c.i]) == "entity") {
                    if (!c.Has(3)) { c.Incomplete(); return; }
                    const std::string& selector = args[c.i + 1];
                    bool eyes = false;
                    if (!ParseAnchor(c, args[c.i + 2], eyes)) return;
                    c.i += 3;
                    Sources next;
                    for (const CommandSourceStack& source : sources) {
                        std::vector<SelectedEntity> found;
                        if (!OptionalEntities(c, selector, source, found)) return;
                        for (const SelectedEntity& e : found) next.push_back(source.Facing(e, eyes));
                    }
                    sources = std::move(next);
                    if (!c.WithinForkLimit(sources.size())) return;
                    continue;
                }
                if (!c.Has(3)) { c.Incomplete(); return; }
                Sources next;
                for (const CommandSourceStack& source : sources) {
                    glm::dvec3 target; std::string error;
                    if (!ParseVec3(args[c.i], args[c.i + 1], args[c.i + 2], source, source.rotation, target, error)) { c.Fail(error); return; }
                    next.push_back(source.Facing(target));
                }
                c.i += 3;
                sources = std::move(next);
                if (!c.WithinForkLimit(sources.size())) return;
                continue;
            }

            // ── align <axes> ────────────────────────────────────────────────
            if (word == "align") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                const std::string axes = Lower(c.Next());
                bool ax = false, ay = false, az = false;
                for (char ch : axes) {
                    bool* flag = ch == 'x' ? &ax : ch == 'y' ? &ay : ch == 'z' ? &az : nullptr;
                    // SwizzleArgument: only x/y/z, each at most once.
                    if (!flag || *flag) { c.Fail("Invalid swizzle, expected combination of 'x', 'y' and 'z'"); return; }
                    *flag = true;
                }
                for (CommandSourceStack& source : sources) {
                    glm::dvec3 p = source.position;
                    if (ax) p.x = std::floor(p.x);
                    if (ay) p.y = std::floor(p.y);
                    if (az) p.z = std::floor(p.z);
                    source = source.WithPosition(p);
                }
                continue;
            }

            // ── anchored <feet|eyes> ────────────────────────────────────────
            if (word == "anchored") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                bool eyes = false;
                if (!ParseAnchor(c, c.Next(), eyes)) return;
                for (CommandSourceStack& source : sources) source = source.WithAnchor(eyes);
                continue;
            }

            // ── in <dimension> ──────────────────────────────────────────────
            if (word == "in") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                const std::string& name = c.Next();
                const auto dim = DimensionCommand::ParseDimension(name);
                if (!dim) { c.Fail("Unknown dimension '" + name + "'"); return; }
                // GetOrCreate: in MC every dimension always exists, so
                // `/execute in the_nether run tp ...` works before anyone has
                // been there. Here a level is created on first use; refusing
                // an uncreated one made the command fail for any dimension
                // nobody had visited yet.
                if (!g_integratedServer || !g_integratedServer->GetOrCreateLevel(*dim)) { c.Fail("That dimension is not loaded"); return; }
                for (CommandSourceStack& source : sources) source = source.WithLevel(*dim);
                continue;
            }

            // ── summon <entity> ─────────────────────────────────────────────
            if (word == "summon") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                const std::string& name = c.Next();
                Game::EntityTypeId type{};
                if (!SummonCommand::ParseEntityType(name, type)) { c.Fail("Can't find element '" + name + "' of type 'minecraft:entity_type'"); return; }
                if (!g_integratedServer) return;
                Sources next;
                for (const CommandSourceStack& source : sources) {
                    // MC spawnEntityAndRedirect: SummonCommand.createEntity at
                    // the source's position, then withEntity(it).
                    std::vector<int32_t> ids;
                    g_integratedServer->SummonMobs(type, source.position, 1, {}, source.dimension, &ids);
                    ServerLevel* level = LevelOf(source);
                    Game::Mob* mob = (level && level->Mobs() && !ids.empty()) ? level->Mobs()->Find(ids.front()) : nullptr;
                    SelectedEntity described;
                    if (!mob || !DescribeEntity(mob, source, described)) { c.Fail("Unable to summon entity"); return; }
                    next.push_back(source.WithEntity(described));
                }
                sources = std::move(next);
                if (!c.WithinForkLimit(sources.size())) return;
                continue;
            }

            // ── on <relation> ───────────────────────────────────────────────
            if (word == "on") {
                if (!c.Has(1)) { c.Incomplete(); return; }
                if (!ForkRelation(c, Lower(c.Next()), sources)) return;
                continue;
            }

            // ── if / unless ─────────────────────────────────────────────────
            if (word == "if" || word == "unless") {
                if (!Conditional(c, word == "if", sources)) return;
                // A terminal test reported itself and cleared the sources.
                if (c.i >= args.size()) return;
                continue;
            }

            // ── store — no scoreboard, bossbars, NBT or storage ─────────────
            if (word == "store") {
                c.Fail("'store' is not available in this game (no scoreboard, boss bars, NBT or command storage)");
                return;
            }

            c.Fail("Unknown or incomplete command, see below for error");
            c.Fail("... " + word + " <--[HERE]");
            return;
        }

        // Ran out of tokens after a modifier with no `run`: vanilla says the
        // command is incomplete.
        c.Incomplete();
    }

} // namespace Server
