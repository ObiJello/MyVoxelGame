// File: src/server/level/LocateFinder.cpp
#include "LocateFinder.hpp"
#include "ServerLevel.hpp"
#include "server/world/MyTerrainGenerator.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "levelgen/Heightmap.h"

#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/structure/StructurePlacement.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/Structures.h"
#include "levelgen/structure/StructureStartData.h"
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
#include <limits>
#include <map>
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
                if (std::find(structureIds.begin(), structureIds.end(), entry.structure->name) != structureIds.end()) {
                    scan.wanted.push_back(entry.structure);
                }
            }
            if (!scan.wanted.empty()) scans.push_back(std::move(scan));
        }
        if (scans.empty()) return std::nullopt;

        std::optional<LocateResult> nearest;
        double nearestDistSqr = std::numeric_limits<double>::max();
        std::vector<const Scan*> randomSpread;

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
                    // the click-to-teleport lands on the ground; the sampled
                    // Y is kept when the column cannot be answered.
                    return LocateResult{glm::ivec3(blockX, LocateSurfaceY(level, blockX, blockZ, blockY), blockZ), biome};
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

} // namespace Server
