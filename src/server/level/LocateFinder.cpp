// File: src/server/level/LocateFinder.cpp
#include "LocateFinder.hpp"
#include "ServerLevel.hpp"
#include "server/world/MyTerrainGenerator.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/biome/Biomes.hpp"
#include "levelgen/Heightmap.h"

#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/structure/StructurePlacement.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/Structures.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/TwilightLandmarks.h"
#include "levelgen/structure/TwilightStructurePlacements.h"
#include "levelgen/RandomState.h"
#include "world/biome/BiomeSource.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>

namespace Server {

    namespace {
        namespace mls = minecraft::levelgen::structure;

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        // Where the data pack lives: the same rule the terrain library uses
        // (MC_DATA_ROOT, else ./data).
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        // One tag file's ids, nested "#tags" followed; a missing file is an
        // empty list. `seen` guards against a tag that includes itself.
        void CollectStructureTag(const std::string& tag, std::vector<std::string>& out, std::set<std::string>& seen) {
            std::string id = tag;
            if (!id.empty() && id[0] == '#') id.erase(0, 1);
            id = WithNamespace(id);
            if (!seen.insert(id).second) return;
            const size_t colon = id.find(':');
            const std::filesystem::path file = DataRoot() / id.substr(0, colon) / "tags" / "worldgen" / "structure" /
                                               (id.substr(colon + 1) + ".json");
            std::ifstream in(file);
            if (!in) return;
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false, true);
            if (j.is_discarded() || !j.is_object()) return;
            const auto values = j.find("values");
            if (values == j.end() || !values->is_array()) return;
            for (const auto& v : *values) {
                std::string entry;
                if (v.is_string()) entry = v.get<std::string>();
                else if (v.is_object() && v.contains("id") && v["id"].is_string()) entry = v["id"].get<std::string>();
                if (entry.empty()) continue;
                if (entry[0] == '#') CollectStructureTag(entry, out, seen);
                else if (std::find(out.begin(), out.end(), WithNamespace(entry)) == out.end()) out.push_back(WithNamespace(entry));
            }
        }

        // ChunkGenerator.getStructureGeneratingAt, for one structure: does
        // `info` start in chunk (cx, cz) under `placement`?
        bool StructureGeneratesAt(Game::MyTerrainGenerator& generator, mls::ChunkGeneratorStructureState& state,
                                  const mls::StructurePlacement& placement, const mls::StructureInfo& info,
                                  int32_t cx, int32_t cz) {
            if (!placement.isStructureChunk(state, cx, cz)) return false;
            if (!mls::Structures::isImplemented(info)) return false;
            const auto& validBiomes = mls::BiomeTags::resolve(info.biomesTag);
            mls::GenerationContext context(generator.GetLibGenerator(), generator.GetRandomState(),
                                           state.biomeSource(), state.sampler(), state.getLevelSeed(),
                                           cx, cz, &validBiomes);
            mls::StructureStartData start;
            try {
                return mls::Structures::generate(info, context, 0, start) && start.isValid();
            } catch (const std::exception& e) {
                Log::Warning("[Locate] %s at chunk (%d, %d) threw: %s", info.name.c_str(), cx, cz, e.what());
                return false;
            }
        }

        // StructurePlacement.getLocatePos.
        glm::ivec3 LocatePos(const mls::StructurePlacement& placement, int32_t cx, int32_t cz) {
            return glm::ivec3(cx * 16 + placement.locateOffsetX(),
                              placement.locateOffsetY(),
                              cz * 16 + placement.locateOffsetZ());
        }

        double DistSqr(const glm::ivec3& a, const glm::ivec3& b) {
            const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }
    }

    namespace {
        // Walks a column top-down and returns the standing Y — one above the
        // topmost blocking block — or `fallbackY` for an empty column. With
        // a ceiling the roof is skipped first: leading air, then the solid
        // roof, then the first blocking block that has at least two air
        // blocks above it (a shorter gap is treated as more roof).
        // `blocking(y)` says whether the block at y blocks motion.
        template <typename Blocking>
        int SurfaceOfColumn(int topY, int bottomY, bool hasCeiling, Blocking&& blocking, int fallbackY) {
            if (!hasCeiling) {
                for (int y = topY; y >= bottomY; --y) if (blocking(y)) return y + 1;
                return fallbackY;
            }
            enum { AboveRoof, InRoof, UnderRoof } state = AboveRoof;
            int airRun = 0;
            for (int y = topY; y >= bottomY; --y) {
                const bool solid = blocking(y);
                switch (state) {
                    case AboveRoof: if (solid) state = InRoof; break;
                    case InRoof:    if (!solid) { state = UnderRoof; airRun = 1; } break;
                    case UnderRoof:
                        if (!solid) { ++airRun; break; }
                        if (airRun >= 2) return y + 1;
                        state = InRoof;   // a one-block crack in the roof, keep going
                        break;
                }
            }
            return fallbackY;
        }
    }

    int LocateSurfaceY(ServerLevel& level, int x, int z, int fallbackY) {
        const Game::DimensionId dim = level.Dimension();
        const bool hasCeiling = Game::DimensionHasCeiling(dim);
        const int minY = Game::DimensionMinY(dim);
        const int topY = minY + Game::DimensionLogicalHeight(dim) - 1;

        // A loaded chunk is the real terrain: trees, water, player edits.
        if (Game::World* world = level.World(); world && world->IsChunkLoaded(x >> 4, z >> 4)) {
            if (!hasCeiling) {
                const int top = world->GetSurfaceHeight(x, z, Game::HeightmapType::MotionBlocking);
                return top >= minY ? top + 1 : fallbackY;
            }
            return SurfaceOfColumn(topY, minY, true, [&](int y) {
                return Game::HeightmapIsOpaque(Game::HeightmapType::MotionBlocking, world->GetBlock(x, y, z));
            }, fallbackY);
        }

        // Otherwise the noise column, the way MC's getBaseHeight reads it
        // (no chunk needed; surface rules, carvers and features are not in
        // it, so this is the bare terrain).
        Game::MyTerrainGenerator* generator = level.TerrainGenerator();
        if (!generator || !generator->GetLibGenerator() || !generator->GetRandomState()) return fallbackY;
        if (!hasCeiling) {
            const int surface = generator->SurfaceHeightAt(x, z);
            return surface != INT_MIN ? surface : fallbackY;
        }
        std::vector<minecraft::levelgen::BlockState*> column;
        generator->GetLibGenerator()->getBaseColumn(x, z, generator->GetRandomState(), column);
        if (column.empty()) return fallbackY;
        const int columnMinY = generator->GetLibGenerator()->getBaseColumnMinY();
        const auto opaque = minecraft::levelgen::Heightmap::getOpaquePredicate(minecraft::levelgen::Heightmap::Types::WORLD_SURFACE_WG);
        const int columnTop = columnMinY + static_cast<int>(column.size()) - 1;
        return SurfaceOfColumn(std::min(topY, columnTop), std::max(minY, columnMinY), true, [&](int y) {
            minecraft::levelgen::BlockState* state = column[static_cast<size_t>(y - columnMinY)];
            return state != nullptr && opaque(state);
        }, fallbackY);
    }

    bool IsWorldgenTag(const std::string& kind, const std::string& idOrTag) {
        if (idOrTag.empty()) return false;
        if (idOrTag[0] == '#') return true;
        const std::string id = WithNamespace(idOrTag);
        const size_t colon = id.find(':');
        std::error_code ec;
        return std::filesystem::is_regular_file(
            DataRoot() / id.substr(0, colon) / "tags" / "worldgen" / kind / (id.substr(colon + 1) + ".json"), ec);
    }

    std::vector<std::string> ResolveStructureIdOrTag(const std::string& idOrTag) {
        std::vector<std::string> out;
        if (IsWorldgenTag("structure", idOrTag)) {
            std::set<std::string> seen;
            CollectStructureTag(idOrTag, out, seen);
        } else {
            out.push_back(WithNamespace(idOrTag));
        }
        return out;
    }

    std::unordered_set<std::string> ResolveBiomeIdOrTag(const std::string& idOrTag) {
        if (IsWorldgenTag("biome", idOrTag)) {
            const std::string bare = idOrTag[0] == '#' ? idOrTag.substr(1) : idOrTag;
            try {
                return mls::BiomeTags::resolve("#" + WithNamespace(bare));
            } catch (const std::exception&) {
                return {};
            }
        }
        return { WithNamespace(idOrTag) };
    }

    const std::vector<std::string>& AllBiomeIds() {
        static const std::vector<std::string> ids = [] {
            std::vector<std::string> out;
            for (Game::BiomeId id = 0; id < Game::BiomeRegistry::Count(); ++id) {
                out.push_back(WithNamespace(std::string(Game::BiomeRegistry::Get(id).name)));
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }();
        return ids;
    }

    const std::vector<std::string>& AllStructureIds() {
        static const std::vector<std::string> ids = [] {
            std::vector<std::string> out;
            try {
                for (const mls::StructureInfo* info : mls::StructureSets::allStructures()) out.push_back(info->name);
            } catch (const std::exception& e) {
                Log::Warning("[Locate] structure registry failed to load: %s", e.what());
            }
            std::sort(out.begin(), out.end());
            return out;
        }();
        return ids;
    }

    bool LevelHasBiome(ServerLevel& level, const std::string& biomeId) {
        Game::MyTerrainGenerator* generator = level.TerrainGenerator();
        if (!generator || !generator->GetBiomeSource()) return false;
        return generator->GetBiomeSource()->possibleBiomes().count(biomeId) != 0;
    }

    bool StructureIsGenerated(const std::string& structureId) {
        try {
            return mls::Structures::isImplemented(mls::StructureSets::structureByName(structureId));
        } catch (const std::exception&) {
            return false;
        }
    }

    bool LevelHasStructure(ServerLevel& level, const std::string& structureId) {
        Game::MyTerrainGenerator* generator = level.TerrainGenerator();
        mls::ChunkGeneratorStructureState* state = generator ? generator->GetStructureState() : nullptr;
        if (!state) return false;
        for (const mls::StructureSet* set : state->possibleStructureSets()) {
            for (const auto& entry : set->structures) {
                if (entry.structure->name == structureId) return true;
            }
        }
        return false;
    }

    std::string CanonicalWorldgenId(const std::string& kind, const std::string& typed, ServerLevel* level) {
        // A bare tag ("#in_twilight_forest" — the tab completion shows tags
        // without their namespace): the namespace that defines it, minecraft
        // first. Assuming minecraft: would miss every mod tag.
        if (!typed.empty() && typed[0] == '#' && typed.find(':') == std::string::npos) {
            const std::string path = typed.substr(1);
            std::error_code ec;
            auto defines = [&](const std::filesystem::path& ns) {
                return std::filesystem::is_regular_file(ns / "tags" / "worldgen" / kind / (path + ".json"), ec);
            };
            if (defines(DataRoot() / "minecraft")) return "#minecraft:" + path;
            for (const auto& ns : std::filesystem::directory_iterator(DataRoot(), ec)) {
                if (ns.is_directory(ec) && defines(ns.path())) {
                    return "#" + ns.path().filename().string() + ":" + path;
                }
            }
            return typed;
        }
        if (typed.empty() || typed[0] == '#' || typed.find(':') != std::string::npos) return typed;
        const bool biome = kind == "biome";
        const std::string suffix = ":" + typed;
        auto endsWithPath = [&](const std::string& id) {
            return id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        // (1) What this level generates.
        if (level) {
            if (biome) {
                if (Game::MyTerrainGenerator* g = level->TerrainGenerator(); g && g->GetBiomeSource()) {
                    const auto& possible = g->GetBiomeSource()->possibleBiomes();
                    if (possible.count("minecraft" + suffix)) return "minecraft" + suffix;
                    for (const auto& id : possible) if (endsWithPath(id)) return id;
                }
            } else {
                for (const std::string& id : AllStructureIds()) {
                    if (endsWithPath(id) && LevelHasStructure(*level, id)) return id;
                }
            }
        }
        // (2) minecraft:, (3) any other namespace.
        const std::vector<std::string>& all = biome ? AllBiomeIds() : AllStructureIds();
        if (std::binary_search(all.begin(), all.end(), "minecraft" + suffix)) return "minecraft" + suffix;
        for (const std::string& id : all) if (endsWithPath(id)) return id;
        // (4) A tag of that name in any namespace (minecraft first).
        if (IsWorldgenTag(kind, typed)) return "#minecraft" + suffix;
        std::error_code ec;
        for (const auto& ns : std::filesystem::directory_iterator(DataRoot(), ec)) {
            if (!ns.is_directory(ec)) continue;
            if (std::filesystem::is_regular_file(ns.path() / "tags" / "worldgen" / kind / (typed + ".json"), ec)) {
                return "#" + ns.path().filename().string() + suffix;
            }
        }
        return "minecraft" + suffix;
    }

    std::optional<LocateResult> FindNearestStructure(ServerLevel& level,
                                                     const std::vector<std::string>& structureIds,
                                                     const glm::ivec3& from, int maxSearchRadius) {
        Game::MyTerrainGenerator* generator = level.TerrainGenerator();
        if (!generator || !generator->GetLibGenerator() || !generator->GetRandomState()) return std::nullopt;
        mls::ChunkGeneratorStructureState* state = generator->GetStructureState();
        if (!state) return std::nullopt;

        // getPlacementsForStructure: every possible set (this dimension's
        // biome-filtered list) containing a wanted structure, grouped by
        // placement — the same set object serves as the key.
        struct Scan { const mls::StructureSet* set; std::vector<const mls::StructureInfo*> wanted; };
        std::vector<Scan> scans;
        for (const mls::StructureSet* set : state->possibleStructureSets()) {
            Scan scan{set, {}};
            for (const auto& entry : set->structures) {
                // A structure this build does not generate yet can never be
                // found; skipping it saves scanning a whole radius for it.
                if (std::find(structureIds.begin(), structureIds.end(), entry.structure->name) != structureIds.end() &&
                    mls::Structures::isImplemented(*entry.structure)) {
                    scan.wanted.push_back(entry.structure);
                }
            }
            if (!scan.wanted.empty()) scans.push_back(std::move(scan));
        }
        if (scans.empty()) return std::nullopt;

        std::optional<LocateResult> nearest;
        double nearestDistSqr = std::numeric_limits<double>::max();
        std::vector<const Scan*> randomSpread;
        std::vector<const Scan*> landmarks;

        for (const Scan& scan : scans) {
            const mls::StructurePlacement* placement = scan.set->placement.get();
            if (const auto* rings = dynamic_cast<const mls::ConcentricRingsStructurePlacement*>(placement)) {
                // getNearestGeneratedStructure(rings): the precomputed
                // positions, measured to the chunk centre at y=32, the closest
                // one that generates.
                state->ensureStructuresGenerated();
                const auto* positions = state->getRingPositionsFor(rings);
                if (!positions) continue;
                std::optional<LocateResult> closest;
                double closestDist = std::numeric_limits<double>::max();
                for (const auto& [cx, cz] : *positions) {
                    const glm::ivec3 probe{cx * 16 + 8, 32, cz * 16 + 8};
                    const double d = DistSqr(probe, from);
                    if (closest && d >= closestDist) continue;
                    for (const mls::StructureInfo* info : scan.wanted) {
                        if (StructureGeneratesAt(*generator, *state, *placement, *info, cx, cz)) {
                            closest = LocateResult{LocatePos(*placement, cx, cz), info->name};
                            closestDist = d;
                            break;
                        }
                    }
                }
                if (closest) {
                    const double d = DistSqr(closest->pos, from);
                    if (d < nearestDistSqr) { nearestDistSqr = d; nearest = closest; }
                }
            } else if (dynamic_cast<const mls::RandomSpreadStructurePlacement*>(placement)) {
                randomSpread.push_back(&scan);
            } else if (dynamic_cast<const mls::TwilightLandmarkGridPlacement*>(placement)) {
                landmarks.push_back(&scan);
            }
        }

        // Twilight Forest landmarks (lich tower, hydra lair, labyrinth…):
        // vanilla's findNearestMapStructure skips a placement that is neither
        // rings nor random spread; the mod patches the tail of it with
        // WorldUtil.findNearestMapLandmark — every landmark centre (one per
        // 256-block region, LegacyLandmarkPlacements.landmarkCenterScanner)
        // within the radius, counted when the placement accepts its chunk and
        // the biome at the centre is one of the structure's, the nearest
        // (horizontally) kept. Same here, walking regions ring by ring and
        // stopping once no farther ring can beat the best; the kept candidate
        // is also run through the structure's own generation (as the other
        // placements are), falling back to the next-nearest if it declines.
        if (!landmarks.empty()) {
            const int32_t focusChunkX = (static_cast<int32_t>(std::floor(from.x / 16.0))) & ~15;
            const int32_t focusChunkZ = (static_cast<int32_t>(std::floor(from.z / 16.0))) & ~15;
            struct Candidate { double distSqr; int32_t cx, cz, blockX, blockZ; const mls::StructurePlacement* placement; const mls::StructureInfo* info; };
            auto farther = [](const Candidate& a, const Candidate& b) { return a.distSqr > b.distSqr; };
            std::priority_queue<Candidate, std::vector<Candidate>, decltype(farther)> pending(farther);
            minecraft::world::biome::BiomeSource* source = state->biomeSource();
            const auto* sampler = state->sampler();
            auto consider = [&](int dx, int dz) {
                const minecraft::core::BlockPos center = mls::twilight_landmarks::getNearestCenterXZ(
                    focusChunkX + dx * 16, focusChunkZ + dz * 16);
                const int32_t cx = center.getX() >> 4, cz = center.getZ() >> 4;
                std::string biome;
                for (const Scan* scan : landmarks) {
                    const mls::StructurePlacement& placement = *scan->set->placement;
                    if (!placement.isStructureChunk(*state, cx, cz)) continue;
                    if (biome.empty() && source && sampler) {
                        biome = source->getNoiseBiome(center.getX() >> 2, center.getY() >> 2, center.getZ() >> 2, *sampler);
                    }
                    for (const mls::StructureInfo* info : scan->wanted) {
                        if (!mls::BiomeTags::resolve(info->biomesTag).count(biome)) continue;
                        const double ddx = center.getX() - from.x, ddz = center.getZ() - from.z;
                        pending.push(Candidate{ddx * ddx + ddz * ddz, cx, cz, center.getX(), center.getZ(), &placement, info});
                    }
                }
            };
            std::optional<LocateResult> best;
            for (int ring = 0; ring <= maxSearchRadius && !best; ++ring) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    for (int dz = -ring; dz <= ring; ++dz) {
                        if (std::abs(dx) != ring && std::abs(dz) != ring) continue;
                        consider(dx, dz);
                    }
                }
                // A centre in ring k sits within +/-3 chunks of its region's
                // middle and the player anywhere in the focus region's 16
                // chunks, so it is at least 256k - 304 blocks away; a
                // candidate closer than ring+1's floor (rounded down to
                // 256(k+1) - 320) cannot be beaten by any farther ring.
                const double floorNext = std::max(0.0, static_cast<double>(ring + 1) * 256.0 - 320.0);
                const bool lastRing = ring == maxSearchRadius;
                while (!pending.empty() && (lastRing || pending.top().distSqr <= floorNext * floorNext)) {
                    const Candidate c = pending.top();
                    pending.pop();
                    if (StructureGeneratesAt(*generator, *state, *c.placement, *c.info, c.cx, c.cz)) {
                        best = LocateResult{glm::ivec3(c.blockX, 0, c.blockZ), c.info->name};   // the landmark centre, as the mod reports
                        break;
                    }
                }
            }
            if (best) {
                const double d = DistSqr(best->pos, from);
                if (d < nearestDistSqr) { nearestDistSqr = d; nearest = best; }
            }
        }

        if (!randomSpread.empty()) {
            const int32_t originChunkX = static_cast<int32_t>(std::floor(from.x / 16.0));
            const int32_t originChunkZ = static_cast<int32_t>(std::floor(from.z / 16.0));
            for (int radius = 0; radius <= maxSearchRadius; ++radius) {
                bool foundSomething = false;
                for (const Scan* scan : randomSpread) {
                    const auto* placement = static_cast<const mls::RandomSpreadStructurePlacement*>(scan->set->placement.get());
                    const int32_t spacing = placement->spacing();
                    // getNearestGeneratedStructure(random spread): the ring of
                    // spacing cells at this radius, the first hit returned.
                    std::optional<LocateResult> hit;
                    for (int x = -radius; x <= radius && !hit; ++x) {
                        const bool xEdge = x == -radius || x == radius;
                        for (int z = -radius; z <= radius && !hit; ++z) {
                            const bool zEdge = z == -radius || z == radius;
                            if (!xEdge && !zEdge) continue;
                            const auto [cx, cz] = placement->getPotentialStructureChunk(
                                state->getLevelSeed(), originChunkX + spacing * x, originChunkZ + spacing * z);
                            for (const mls::StructureInfo* info : scan->wanted) {
                                if (StructureGeneratesAt(*generator, *state, *placement, *info, cx, cz)) {
                                    hit = LocateResult{LocatePos(*placement, cx, cz), info->name};
                                    break;
                                }
                            }
                        }
                    }
                    if (hit) {
                        foundSomething = true;
                        const double d = DistSqr(hit->pos, from);
                        if (d < nearestDistSqr) { nearestDistSqr = d; nearest = hit; }
                    }
                }
                if (foundSomething) return nearest;
            }
        }
        return nearest;
    }

    std::string BiomeAt(ServerLevel& level, const glm::ivec3& pos) {
        Game::MyTerrainGenerator* generator = level.TerrainGenerator();
        if (!generator || !generator->GetBiomeSource() || !generator->GetRandomState() ||
            !generator->GetRandomState()->sampler()) return {};
        return generator->GetBiomeSource()->getNoiseBiome(pos.x >> 2, pos.y >> 2, pos.z >> 2,
                                                          *generator->GetRandomState()->sampler());
    }

    namespace {
        // The standing Y nearest `aroundY` in column (x, z) whose feet block
        // is in the wanted biome: a motion-blocking floor with two
        // non-blocking (air, not water) blocks over it, searched outward up
        // to 64 blocks each way. A loaded chunk answers from its blocks,
        // otherwise the generator's noise column (cheese / spaghetti caves
        // and aquifers are in it; carvers are not). `aroundY` when nothing
        // fits.
        template <typename InBiome>
        int OpenFloorInBiome(ServerLevel& level, int x, int z, int aroundY, InBiome&& inBiome) {
            const Game::DimensionId dim = level.Dimension();
            const int minY = Game::DimensionMinY(dim);
            const int maxY = minY + Game::DimensionLogicalHeight(dim) - 2;
            std::function<bool(int)> blocking;
            std::vector<minecraft::levelgen::BlockState*> column;
            int columnMinY = 0;
            if (Game::World* world = level.World(); world && world->IsChunkLoaded(x >> 4, z >> 4)) {
                blocking = [world, x, z](int y) {
                    return Game::HeightmapIsOpaque(Game::HeightmapType::MotionBlocking, world->GetBlock(x, y, z));
                };
            } else {
                Game::MyTerrainGenerator* generator = level.TerrainGenerator();
                if (!generator || !generator->GetLibGenerator() || !generator->GetRandomState()) return aroundY;
                generator->GetLibGenerator()->getBaseColumn(x, z, generator->GetRandomState(), column);
                if (column.empty()) return aroundY;
                columnMinY = generator->GetLibGenerator()->getBaseColumnMinY();
                const auto opaque = minecraft::levelgen::Heightmap::getOpaquePredicate(
                    minecraft::levelgen::Heightmap::Types::MOTION_BLOCKING);
                blocking = [&column, columnMinY, opaque](int y) {
                    const int i = y - columnMinY;
                    if (i < 0 || i >= static_cast<int>(column.size())) return false;
                    minecraft::levelgen::BlockState* state = column[static_cast<size_t>(i)];
                    return state != nullptr && opaque(state);
                };
            }
            auto standable = [&](int y) {
                return y - 1 >= minY && y + 1 <= maxY && blocking(y - 1) && !blocking(y) && !blocking(y + 1) && inBiome(y);
            };
            for (int d = 0; d <= 64; ++d) {
                if (standable(aroundY - d)) return aroundY - d;
                if (d > 0 && standable(aroundY + d)) return aroundY + d;
            }
            return aroundY;
        }
    }

    std::optional<LocateResult> FindClosestBiome(ServerLevel& level,
                                                 const std::unordered_set<std::string>& biomeIds,
                                                 const glm::ivec3& from, int maxSearchRadius,
                                                 int sampleResolutionHorizontal, int sampleResolutionVertical) {
        Game::MyTerrainGenerator* generator = level.TerrainGenerator();
        if (!generator || !generator->GetBiomeSource() || !generator->GetRandomState() ||
            !generator->GetRandomState()->sampler()) return std::nullopt;
        minecraft::world::biome::BiomeSource& source = *generator->GetBiomeSource();
        const auto& sampler = *generator->GetRandomState()->sampler();

        // possibleBiomes ∩ wanted.
        std::unordered_set<std::string> candidates;
        for (const auto& key : source.possibleBiomes()) if (biomeIds.count(key)) candidates.insert(key);
        if (candidates.empty()) return std::nullopt;

        // Mth.outFromOrigin(y, minY + 1, maxY + 1, step): the origin first,
        // then alternately above and below, within the level's bounds.
        const int minY = Game::DimensionMinY(level.Dimension()) + 1;
        const int maxY = Game::DimensionMinY(level.Dimension()) + Game::DimensionLogicalHeight(level.Dimension());
        std::vector<int> sampleYs;
        {
            const int origin = std::clamp(from.y, minY, maxY);
            sampleYs.push_back(origin);
            for (int k = 1;; ++k) {
                const int up = origin + k * sampleResolutionVertical, down = origin - k * sampleResolutionVertical;
                if (up > maxY && down < minY) break;
                if (up <= maxY) sampleYs.push_back(up);
                if (down >= minY) sampleYs.push_back(down);
            }
        }

        // BlockPos.spiralAround(ZERO, sampleRadius, EAST, SOUTH): the origin,
        // then a square spiral — east, south, west, north — one lattice cell
        // per step, out to the radius.
        const int sampleRadius = static_cast<int>(std::floor(static_cast<double>(maxSearchRadius) / sampleResolutionHorizontal));
        auto sampleColumn = [&](int sx, int sz) -> std::optional<LocateResult> {
            const int blockX = from.x + sx * sampleResolutionHorizontal;
            const int blockZ = from.z + sz * sampleResolutionHorizontal;
            const int quartX = blockX >> 2, quartZ = blockZ >> 2;
            for (int blockY : sampleYs) {
                const std::string biome = source.getNoiseBiome(quartX, blockY >> 2, quartZ, sampler);
                if (candidates.count(biome)) {
                    // MC reports the Y of the matching SAMPLE, which is often
                    // underground or in the air (biomes are 3D). This game
                    // reports the terrain surface of that column instead, so
                    // the click-to-teleport lands on the ground — unless the
                    // surface is another biome (a cave biome: crystal
                    // caverns, lush caves, deep dark), where it is the open
                    // floor nearest the sample inside the biome. The sampled
                    // Y is kept when the column cannot be answered.
                    const int surfaceY = LocateSurfaceY(level, blockX, blockZ, blockY);
                    if (source.getNoiseBiome(quartX, surfaceY >> 2, quartZ, sampler) == biome) {
                        return LocateResult{glm::ivec3(blockX, surfaceY, blockZ), biome};
                    }
                    const int floorY = OpenFloorInBiome(level, blockX, blockZ, blockY, [&](int y) {
                        return source.getNoiseBiome(quartX, y >> 2, quartZ, sampler) == biome;
                    });
                    return LocateResult{glm::ivec3(blockX, floorY, blockZ), biome};
                }
            }
            return std::nullopt;
        };
        if (auto r = sampleColumn(0, 0)) return r;
        int x = 0, z = 0;
        const int dx[4] = {1, 0, -1, 0}, dz[4] = {0, 1, 0, -1};   // east, south, west, north
        int dir = 0;
        for (int leg = 1; leg <= 2 * sampleRadius; ++leg) {
            for (int side = 0; side < 2; ++side) {
                for (int step = 0; step < leg; ++step) {
                    x += dx[dir]; z += dz[dir];
                    if (std::abs(x) <= sampleRadius && std::abs(z) <= sampleRadius) {
                        if (auto r = sampleColumn(x, z)) return r;
                    }
                }
                dir = (dir + 1) % 4;
            }
        }
        return std::nullopt;
    }

    const DimensionWorldgenIds& WorldgenIdsFor(ServerLevel& level) {
        // Static data per dimension: the generator's biome source and
        // structure sets and the data/ tags never change while running.
        static std::map<int, DimensionWorldgenIds> cache;
        const int key = static_cast<int>(Game::DimensionToRaw(level.Dimension()));
        if (auto it = cache.find(key); it != cache.end()) return it->second;

        DimensionWorldgenIds ids;
        if (Game::MyTerrainGenerator* g = level.TerrainGenerator(); g && g->GetBiomeSource()) {
            for (const auto& id : g->GetBiomeSource()->possibleBiomes()) ids.biomes.push_back(id);
        }
        for (const std::string& id : AllStructureIds()) {
            if (LevelHasStructure(level, id) && StructureIsGenerated(id)) ids.structures.push_back(id);
        }
        const std::unordered_set<std::string> biomeSet(ids.biomes.begin(), ids.biomes.end());
        const std::unordered_set<std::string> structureSet(ids.structures.begin(), ids.structures.end());

        // Every tag under data/<ns>/tags/worldgen/<kind>/, kept when one of
        // its members lives here.
        auto collectTags = [&](const char* kind, std::vector<std::string>& out) {
            std::error_code ec;
            for (const auto& ns : std::filesystem::directory_iterator(DataRoot(), ec)) {
                if (!ns.is_directory(ec)) continue;
                const std::filesystem::path dir = ns.path() / "tags" / "worldgen" / kind;
                for (auto e = std::filesystem::recursive_directory_iterator(dir, ec);
                     !ec && e != std::filesystem::recursive_directory_iterator(); e.increment(ec)) {
                    if (!e->is_regular_file(ec) || e->path().extension() != ".json") continue;
                    std::string path = std::filesystem::relative(e->path(), dir, ec).replace_extension().generic_string();
                    const std::string tag = "#" + ns.path().filename().string() + ":" + path;
                    bool here = false;
                    if (std::string(kind) == "biome") {
                        for (const auto& member : ResolveBiomeIdOrTag(tag)) {
                            if (biomeSet.count(member)) { here = true; break; }
                        }
                    } else {
                        for (const auto& member : ResolveStructureIdOrTag(tag)) {
                            if (structureSet.count(member)) { here = true; break; }
                        }
                    }
                    if (here) out.push_back(tag);
                }
                ec.clear();
            }
        };
        collectTags("biome", ids.biomeTags);
        collectTags("structure", ids.structureTags);

        for (auto* v : {&ids.biomes, &ids.structures, &ids.biomeTags, &ids.structureTags}) {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        Log::Info("[Locate] %s: %zu biomes, %zu structures, %zu + %zu tags for completion",
                  std::string(Game::DimensionName(level.Dimension())).c_str(), ids.biomes.size(),
                  ids.structures.size(), ids.biomeTags.size(), ids.structureTags.size());
        return cache.emplace(key, std::move(ids)).first->second;
    }

} // namespace Server
