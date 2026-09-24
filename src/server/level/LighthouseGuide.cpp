// File: src/server/level/LighthouseGuide.cpp
//
// See LighthouseGuide.hpp.
#include "server/level/LighthouseGuide.hpp"

#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/MyTerrainGenerator.hpp"

#include "common/core/Log.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/entity/HushLighthouseLampBlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"

#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/structure/StructurePlacement.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/Structures.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/RandomState.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace Server::LighthouseGuide {

    namespace {

        namespace mls = minecraft::levelgen::structure;

        constexpr const char* kAurelith = "minecraft:aurelith";
        // The start chunk sits a piece's half-width (and a rotation) off the
        // Heart; a cell is tried when its potential chunk is this much past
        // the guide range, so a Heart just inside the range is never missed.
        constexpr double kChunkMargin = 48.0;

        struct Lamp { Game::DimensionId dim; glm::ivec3 pos; };

        // Lamps waiting for an answer, and the ones already queued (a chunk
        // that reloads before its lamp was answered must not queue it twice).
        std::deque<Lamp>& Queue() {
            static std::deque<Lamp> q;
            return q;
        }
        std::set<std::tuple<int, int, int, int>>& Queued() {
            static std::set<std::tuple<int, int, int, int>> s;
            return s;
        }
        std::tuple<int, int, int, int> KeyOf(const Lamp& l) {
            return {static_cast<int>(l.dim), l.pos.x, l.pos.y, l.pos.z};
        }

        // Potential start chunk → the Heart's x/z (nullopt: no city starts
        // there). Per dimension, for the session; the seed fixes it.
        std::map<std::tuple<int, int32_t, int32_t>, std::optional<glm::ivec2>>& CityCache() {
            static std::map<std::tuple<int, int32_t, int32_t>, std::optional<glm::ivec2>> c;
            return c;
        }

        // The Heart of the Aurelith starting in chunk (cx, cz), if one does.
        // Structures::generate is the very call a chunk reaching
        // STRUCTURE_STARTS makes, so "would start here" is "does".
        std::optional<glm::ivec2> CityAt(Game::MyTerrainGenerator& generator,
                                         mls::ChunkGeneratorStructureState& state,
                                         const mls::StructurePlacement& placement,
                                         const mls::StructureInfo& info, int32_t cx, int32_t cz) {
            if (!placement.isStructureChunk(state, cx, cz)) return std::nullopt;
            if (!mls::Structures::isImplemented(info)) return std::nullopt;
            const auto& validBiomes = mls::BiomeTags::resolve(info.biomesTag);
            mls::GenerationContext context(generator.GetLibGenerator(), generator.GetRandomState(),
                                           state.biomeSource(), state.sampler(), state.getLevelSeed(),
                                           cx, cz, &validBiomes);
            mls::StructureStartData start;
            try {
                if (!mls::Structures::generate(info, context, 0, start) || !start.isValid()) {
                    return std::nullopt;
                }
            } catch (const std::exception& e) {
                Log::Warning("[LighthouseGuide] %s at chunk (%d, %d) threw: %s",
                             info.name.c_str(), cx, cz, e.what());
                return std::nullopt;
            }
            // The start piece is the centre column (gen_aurelith.py: the
            // start is u_3_3, the Heart at its middle); it is the first piece
            // a jigsaw start holds. Prefer it by name when the detail line
            // carries the template, else the first piece.
            const mls::StructurePieceData* startPiece = &start.pieces.front();
            for (const auto& piece : start.pieces) {
                if (piece.detail.find("aurelith/u_3_3") != std::string::npos) { startPiece = &piece; break; }
            }
            return glm::ivec2(startPiece->boundingBox.centerX(), startPiece->boundingBox.centerZ());
        }

        // The nearest Heart within kGuideRange of `lamp`, or nullopt. False
        // when the level cannot answer at all (no generator yet) — the lamp
        // stays unchecked and is tried again when its chunk next loads.
        bool NearestCity(ServerLevel& level, const glm::ivec3& lamp, std::optional<glm::ivec2>& out) {
            out.reset();
            Game::MyTerrainGenerator* generator = level.TerrainGenerator();
            if (!generator || !generator->GetLibGenerator() || !generator->GetRandomState()) return false;
            mls::ChunkGeneratorStructureState* state = generator->GetStructureState();
            if (!state) return false;

            const mls::RandomSpreadStructurePlacement* placement = nullptr;
            const mls::StructureInfo* info = nullptr;
            for (const mls::StructureSet* set : state->possibleStructureSets()) {
                for (const auto& entry : set->structures) {
                    if (entry.structure && entry.structure->name == kAurelith) {
                        placement = dynamic_cast<const mls::RandomSpreadStructurePlacement*>(set->placement.get());
                        info = entry.structure;
                        break;
                    }
                }
                if (info) break;
            }
            // Not a level Aurelith generates in: every lamp here is checked
            // with no guide.
            if (!placement || !info) return true;

            const int32_t spacing = placement->spacing();
            const double reach = kGuideRange + kChunkMargin;
            const int32_t lampCx = static_cast<int32_t>(std::floor(lamp.x / 16.0));
            const int32_t lampCz = static_cast<int32_t>(std::floor(lamp.z / 16.0));
            const int32_t reachChunks = static_cast<int32_t>(std::ceil(reach / 16.0));
            // Every spacing cell the reach square touches: one potential
            // start chunk each (getPotentialStructureChunk floors the chunk it
            // is given to its cell).
            auto floorDiv = [](int32_t a, int32_t b) {
                return static_cast<int32_t>(std::floor(static_cast<double>(a) / b));
            };
            std::set<std::pair<int32_t, int32_t>> cells;
            for (int32_t rx = floorDiv(lampCx - reachChunks, spacing);
                 rx <= floorDiv(lampCx + reachChunks, spacing); ++rx) {
                for (int32_t rz = floorDiv(lampCz - reachChunks, spacing);
                     rz <= floorDiv(lampCz + reachChunks, spacing); ++rz) {
                    cells.insert(placement->getPotentialStructureChunk(state->getLevelSeed(),
                                                                       rx * spacing, rz * spacing));
                }
            }

            double best = kGuideRange;
            const int dimKey = static_cast<int>(level.Dimension());
            for (const auto& [cx, cz] : cells) {
                const double dx = cx * 16.0 + 8.0 - (lamp.x + 0.5);
                const double dz = cz * 16.0 + 8.0 - (lamp.z + 0.5);
                if (std::hypot(dx, dz) > reach) continue;
                auto key = std::make_tuple(dimKey, cx, cz);
                auto it = CityCache().find(key);
                if (it == CityCache().end()) {
                    it = CityCache().emplace(key, CityAt(*generator, *state, *placement, *info, cx, cz)).first;
                }
                if (!it->second) continue;
                const glm::ivec2 heart = *it->second;
                const double d = std::hypot(heart.x + 0.5 - (lamp.x + 0.5), heart.y + 0.5 - (lamp.z + 0.5));
                if (d <= best) { best = d; out = heart; }
            }
            return true;
        }

    } // namespace

    void NoteChunkLoaded(ServerLevel& level, Game::Math::ChunkPos pos, const Game::Chunk& chunk) {
        (void)pos;
        if (level.Dimension() != Game::DimensionId::Hush) return;
        for (const auto& [local, be] : chunk.GetAllBlockEntities()) {
            (void)local;
            if (!be || be->GetBlockId() != Game::BlockID::HushLighthouseLamp) continue;
            const auto* lamp = dynamic_cast<const Game::HushLighthouseLampBlockEntity*>(be.get());
            if (!lamp || lamp->GuideChecked()) continue;
            const Lamp entry{level.Dimension(), be->GetWorldPos()};
            if (Queued().insert(KeyOf(entry)).second) Queue().push_back(entry);
        }
    }

    namespace {
        // Lamps that arrive without a chunk load — placed by a player, set by
        // a command — are found by a slow sweep of the chunks round each
        // player in the Hush: every kSweepTicks, kSweepRadius chunks each way.
        constexpr int kSweepTicks  = 200;
        constexpr int kSweepRadius = 4;

        void SweepAroundPlayers(ServerLevel& level) {
            static int s_countdown = kSweepTicks;
            if (--s_countdown > 0) return;
            s_countdown = kSweepTicks;
            IntegratedServer* server = g_integratedServer.get();
            PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
            Game::World* world = level.World();
            if (!sessions || !world) return;
            const int dimension = static_cast<int>(level.Dimension());
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session || session->GetDimensionId() != dimension) continue;
                const ServerPlayer* player = session->GetPlayer();
                if (!player) continue;
                const glm::dvec3 p = player->getPosition();
                const int cx = static_cast<int>(std::floor(p.x / 16.0));
                const int cz = static_cast<int>(std::floor(p.z / 16.0));
                for (int dx = -kSweepRadius; dx <= kSweepRadius; ++dx) {
                    for (int dz = -kSweepRadius; dz <= kSweepRadius; ++dz) {
                        if (auto chunk = world->GetLoadedChunk(cx + dx, cz + dz)) {
                            NoteChunkLoaded(level, Game::Math::ChunkPos{cx + dx, cz + dz}, *chunk);
                        }
                    }
                }
            }
        }
    } // namespace

    void Tick(ServerLevel& level) {
        if (level.Dimension() != Game::DimensionId::Hush) return;
        SweepAroundPlayers(level);
        auto& queue = Queue();
        // One lamp per tick: a lookup may generate a city's layout.
        for (size_t tries = queue.size(); tries > 0; --tries) {
            const Lamp lamp = queue.front();
            queue.pop_front();
            if (lamp.dim != level.Dimension()) {
                queue.push_back(lamp);                // another level's turn
                continue;
            }
            Queued().erase(KeyOf(lamp));
            Game::World* world = level.World();
            if (!world) return;
            auto* be = dynamic_cast<Game::HushLighthouseLampBlockEntity*>(world->GetBlockEntity(lamp.pos));
            if (!be || be->GuideChecked()) continue;  // unloaded, broken, or answered: next
            std::optional<glm::ivec2> heart;
            if (!NearestCity(level, lamp.pos, heart)) return;   // no generator yet
            be->SetGuide(heart.has_value(), heart ? heart->x : 0, heart ? heart->y : 0);
            world->BlockEntityChanged(lamp.pos);
            if (heart) {
                Log::Info("[LighthouseGuide] lamp (%d, %d, %d) guides toward Aurelith (%d, %d), %.0f blocks",
                          lamp.pos.x, lamp.pos.y, lamp.pos.z, heart->x, heart->y,
                          std::hypot(heart->x - lamp.pos.x, heart->y - lamp.pos.z));
            }
            return;
        }
    }

} // namespace Server::LighthouseGuide
