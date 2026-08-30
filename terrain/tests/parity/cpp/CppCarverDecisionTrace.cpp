/**
 * C++ Carver Decision Trace
 * Traces exactly which positions are carved and why
 * For comparison with Java to find the divergence point
 */

#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <set>
#include <map>

#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/Blender.h"
#include "levelgen/Beardifier.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/carver/CaveWorldCarver.h"
#include "levelgen/carver/CarvingContext.h"
#include "levelgen/carver/CarvingMask.h"
#include "levelgen/carver/ConfiguredWorldCarver.h"
#include "world/ProtoChunk.h"
#include "world/level/block/Blocks.h"
#include "random/LegacyRandomSource.h"
#include "random/RandomSupport.h"
#include "core/SectionPos.h"
#include "math/Mth.h"

using namespace minecraft;
using namespace minecraft::levelgen;
using namespace minecraft::levelgen::carver;

// Global trace output
std::ofstream g_traceFile;

// Track all carving decisions
struct CarveDecision {
    int x, y, z;
    std::string reason;  // "carved", "skip_sphere", "skip_floor", "skip_mask", "skip_replace", "skip_state"
    std::string blockBefore;
    std::string blockAfter;
};

std::vector<CarveDecision> g_decisions;

// Custom skip checker that logs decisions
bool tracingSkipChecker(
    const CarvingContext& context,
    double xd, double yd, double zd,
    int32_t worldY,
    double floorLevel,
    int worldX, int worldZ
) {
    // Check floor level first
    if (yd <= floorLevel) {
        g_traceFile << "    SKIP floor: (" << worldX << "," << worldY << "," << worldZ
                    << ") yd=" << std::fixed << std::setprecision(6) << yd
                    << " <= floorLevel=" << floorLevel << std::endl;
        return true;
    }

    // Check sphere
    double sphereTest = xd * xd + yd * yd + zd * zd;
    if (sphereTest >= 1.0) {
        g_traceFile << "    SKIP sphere: (" << worldX << "," << worldY << "," << worldZ
                    << ") dist=" << std::fixed << std::setprecision(6) << sphereTest << " >= 1.0" << std::endl;
        return true;
    }

    return false;
}

void traceCarveEllipsoid(
    CarvingContext& context,
    const CaveCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    Aquifer* aquifer,
    double x, double y, double z,
    double horizontalRadius,
    double verticalRadius,
    CarvingMask& mask,
    double floorLevel,
    int ellipsoidId
) {
    ::world::ChunkPos chunkPos = chunk->getPos();
    double centerX = static_cast<double>(chunkPos.getMiddleBlockX());
    double centerZ = static_cast<double>(chunkPos.getMiddleBlockZ());
    double maxDelta = 16.0 + horizontalRadius * 2.0;

    g_traceFile << "\n  Ellipsoid #" << ellipsoidId << ":" << std::endl;
    g_traceFile << "    center=(" << std::fixed << std::setprecision(4) << x << "," << y << "," << z << ")" << std::endl;
    g_traceFile << "    hRadius=" << horizontalRadius << " vRadius=" << verticalRadius << std::endl;
    g_traceFile << "    floorLevel=" << floorLevel << std::endl;

    // Quick bounds check
    if (std::abs(x - centerX) > maxDelta || std::abs(z - centerZ) > maxDelta) {
        g_traceFile << "    SKIP: out of bounds" << std::endl;
        return;
    }

    int32_t chunkMinX = chunkPos.getMinBlockX();
    int32_t chunkMinZ = chunkPos.getMinBlockZ();

    // Calculate bounds within chunk
    int32_t minXIndex = std::max(static_cast<int32_t>(std::floor(x - horizontalRadius)) - chunkMinX - 1, 0);
    int32_t maxXIndex = std::min(static_cast<int32_t>(std::floor(x + horizontalRadius)) - chunkMinX, 15);
    int32_t minY = std::max(static_cast<int32_t>(std::floor(y - verticalRadius)) - 1, context.getMinGenY() + 1);
    int32_t protectedBlocksOnTop = chunk->isUpgrading() ? 0 : 7;
    int32_t maxY = std::min(static_cast<int32_t>(std::floor(y + verticalRadius)) + 1,
                            context.getMinGenY() + context.getGenDepth() - 1 - protectedBlocksOnTop);
    int32_t minZIndex = std::max(static_cast<int32_t>(std::floor(z - horizontalRadius)) - chunkMinZ - 1, 0);
    int32_t maxZIndex = std::min(static_cast<int32_t>(std::floor(z + horizontalRadius)) - chunkMinZ, 15);

    g_traceFile << "    bounds: X[" << minXIndex << "-" << maxXIndex << "] Y[" << minY << "-" << maxY
                << "] Z[" << minZIndex << "-" << maxZIndex << "]" << std::endl;

    int carvedCount = 0;

    for (int32_t xIndex = minXIndex; xIndex <= maxXIndex; ++xIndex) {
        int32_t worldX = chunkPos.getBlockX(xIndex);
        double xd = (static_cast<double>(worldX) + 0.5 - x) / horizontalRadius;

        for (int32_t zIndex = minZIndex; zIndex <= maxZIndex; ++zIndex) {
            int32_t worldZ = chunkPos.getBlockZ(zIndex);
            double zd = (static_cast<double>(worldZ) + 0.5 - z) / horizontalRadius;

            // Check XZ plane first
            if (xd * xd + zd * zd >= 1.0) {
                continue;  // Don't log this - too many
            }

            for (int32_t worldY = maxY; worldY > minY; --worldY) {
                double yd = (static_cast<double>(worldY) - 0.5 - y) / verticalRadius;

                // Check skip conditions
                bool skipFloor = (yd <= floorLevel);
                bool skipSphere = (xd * xd + yd * yd + zd * zd >= 1.0);
                bool skipMask = mask.get(xIndex, worldY, zIndex);

                if (skipFloor || skipSphere || skipMask) {
                    continue;
                }

                // Get current block
                core::BlockPos pos(worldX, worldY, worldZ);
                ::BlockState* blockType = chunk->getBlockState(pos);
                std::string blockName = blockType ? blockType->getIdentifier() : "null";

                // Check if replaceable
                if (configuration.replaceable.count(blockName) == 0) {
                    // Only log interesting positions (near lava level)
                    if (worldY >= -56 && worldY <= -52) {
                        g_traceFile << "    SKIP replace: (" << worldX << "," << worldY << "," << worldZ
                                    << ") block=" << blockName << std::endl;
                    }
                    continue;
                }

                // Get carve state (what block to place)
                int32_t lavaLevel = configuration.lavaLevel.resolveY(context);
                std::string carveBlock;

                if (worldY <= lavaLevel) {
                    carveBlock = "lava";
                } else if (aquifer) {
                    density::DensityFunction::SinglePointContext singleContext(worldX, worldY, worldZ);
                    ::BlockState* aquiferBlock = aquifer->computeSubstance(singleContext, 0.0);
                    if (aquiferBlock == nullptr) {
                        // Only log interesting positions
                        if (worldY >= -56 && worldY <= -52) {
                            g_traceFile << "    SKIP aquifer barrier: (" << worldX << "," << worldY << "," << worldZ << ")" << std::endl;
                        }
                        continue;
                    }
                    if (aquiferBlock->isAir()) {
                        carveBlock = "air";
                    } else if (aquiferBlock->is(::minecraft::world::level::block::Blocks::WATER->defaultBlockState())) {
                        carveBlock = "water";
                    } else if (aquiferBlock->is(::minecraft::world::level::block::Blocks::LAVA->defaultBlockState())) {
                        carveBlock = "lava";
                    } else {
                        carveBlock = "air";  // fallback
                    }
                } else {
                    carveBlock = "air";
                }

                // Log the carve decision (especially near lava level)
                if (worldY >= -56 && worldY <= -52) {
                    g_traceFile << "    CARVE: (" << worldX << "," << worldY << "," << worldZ
                                << ") " << blockName << " -> " << carveBlock
                                << " xd=" << std::fixed << std::setprecision(4) << xd
                                << " yd=" << yd << " zd=" << zd << std::endl;
                }

                // Mark as carved
                mask.set(xIndex, worldY, zIndex);
                carvedCount++;
            }
        }
    }

    g_traceFile << "    Total carved in ellipsoid: " << carvedCount << std::endl;
}

int main(int argc, char* argv[]) {
    const int64_t SEED = 12345;
    const int CHUNK_X = 0;
    const int CHUNK_Z = 0;

    std::cout << "=== C++ Carver Decision Trace ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;

    // Open trace file
    g_traceFile.open("cpp_carver_trace.txt");
    g_traceFile << "=== C++ Carver Decision Trace ===" << std::endl;
    g_traceFile << "Seed: " << SEED << std::endl;
    g_traceFile << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;

    // Initialize systems
    std::cout << "Initializing..." << std::endl;

    NoiseGeneratorSettings* settings = new NoiseGeneratorSettings();
    RandomState* randomState = RandomState::create(settings, SEED);
    FluidPicker* fluidPicker = new OverworldFluidPicker(63, -54,
        ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), ::minecraft::world::level::block::Blocks::LAVA->defaultBlockState());

    // Create chunk
    ::world::ChunkPos chunkPos(CHUNK_X, CHUNK_Z);
    ::BlockState* airBlock = ::minecraft::world::level::block::Blocks::AIR->defaultBlockState();
    ::BlockState* stoneBlock = ::minecraft::world::level::block::Blocks::STONE->defaultBlockState();
    minecraft::world::BlockRegistry* registry = new minecraft::world::BlockRegistry();
    minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
        chunkPos, -64, 384, airBlock, stoneBlock, registry
    );

    // Fill chunk with stone/deepslate (simulating terrain)
    std::cout << "Filling chunk with terrain..." << std::endl;
    for (int x = 0; x < 16; x++) {
        for (int z = 0; z < 16; z++) {
            for (int y = -64; y < 320; y++) {
                if (y < -60) {
                    chunk->setBlockState(x, y, z, ::minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState(), false);
                } else if (y < 0) {
                    chunk->setBlockState(x, y, z, ::minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState(), false);
                } else if (y < 63) {
                    chunk->setBlockState(x, y, z, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), false);
                } else {
                    chunk->setBlockState(x, y, z, ::minecraft::world::level::block::Blocks::AIR->defaultBlockState(), false);
                }
            }
        }
    }

    // Create carving mask
    CarvingMask& mask = chunk->getOrCreateCarvingMask();

    // Create NoiseChunk for aquifer
    NoiseChunk* noiseChunk = NoiseChunk::forChunk(
        chunk, *randomState, Beardifier::EMPTY(), *settings, fluidPicker, Blender::empty()
    );
    Aquifer* aquifer = noiseChunk->aquifer();

    // Create carving context
    CarvingContext carvingContext(
        chunk->getMinBuildHeight(),
        chunk->getMaxBuildHeight() - chunk->getMinBuildHeight(),
        noiseChunk, randomState, nullptr
    );

    // Create cave carver configuration
    std::set<std::string> replaceable = {
        "minecraft:stone", "minecraft:granite", "minecraft:diorite", "minecraft:andesite",
        "minecraft:dirt", "minecraft:coarse_dirt", "minecraft:podzol", "minecraft:grass_block",
        "minecraft:terracotta", "minecraft:white_terracotta", "minecraft:orange_terracotta",
        "minecraft:red_terracotta", "minecraft:brown_terracotta", "minecraft:yellow_terracotta",
        "minecraft:sand", "minecraft:sandstone", "minecraft:red_sand", "minecraft:red_sandstone",
        "minecraft:gravel", "minecraft:mycelium", "minecraft:snow", "minecraft:packed_ice",
        "minecraft:deepslate", "minecraft:tuff", "minecraft:calcite", "minecraft:smooth_basalt",
        "minecraft:clay", "minecraft:dripstone_block", "minecraft:pointed_dripstone"
    };

    static UniformHeight caveHeight(VerticalAnchor::aboveBottom(8), VerticalAnchor::absolute(180));
    static UniformFloat caveYScale(0.1f, 0.9f);
    static UniformFloat caveHorizontalMult(0.7f, 1.4f);
    static UniformFloat caveVerticalMult(0.8f, 1.3f);
    static UniformFloat caveFloorLevel(-1.0f, -0.4f);

    CaveCarverConfiguration caveConfig(
        0.15f, &caveHeight, &caveYScale,
        VerticalAnchor::aboveBottom(8),  // lavaLevel = -56
        CarverDebugSettings(), replaceable,
        &caveHorizontalMult, &caveVerticalMult, &caveFloorLevel
    );

    // Create random source
    LegacyRandomSource legacyRandom(static_cast<int64_t>(RandomSupport::generateUniqueSeed().seedLo));
    int64_t carverSeed = SEED;

    std::cout << "Tracing carvers..." << std::endl;
    g_traceFile << "\n========================================" << std::endl;
    g_traceFile << "Carver Trace" << std::endl;
    g_traceFile << "========================================" << std::endl;

    int totalEllipsoids = 0;

    // Iterate over carving range
    constexpr int32_t CARVER_RANGE = 8;
    for (int32_t dx = -CARVER_RANGE; dx <= CARVER_RANGE; ++dx) {
        for (int32_t dz = -CARVER_RANGE; dz <= CARVER_RANGE; ++dz) {
            ::world::ChunkPos sourcePos(CHUNK_X + dx, CHUNK_Z + dz);

            // Seed the random
            legacyRandom.setLargeFeatureSeed(carverSeed, sourcePos.x(), sourcePos.z());

            // Check if this is a start chunk
            float startCheck = legacyRandom.nextFloat();
            if (startCheck > 0.15f) {
                continue;  // Not a start chunk
            }

            g_traceFile << "\n--- Source chunk (" << sourcePos.x() << ", " << sourcePos.z()
                        << ") isStart (float=" << startCheck << " <= 0.15) ---" << std::endl;

            // Re-seed for carving
            legacyRandom.setLargeFeatureSeed(carverSeed, sourcePos.x(), sourcePos.z());
            legacyRandom.nextFloat();  // consume the isStartChunk check

            // Cave count
            int32_t maxDistance = core::SectionPos::sectionToBlockCoord(8 * 2 - 1);
            int32_t caveCount = legacyRandom.nextInt(legacyRandom.nextInt(legacyRandom.nextInt(15) + 1) + 1);

            g_traceFile << "caveCount=" << caveCount << " maxDistance=" << maxDistance << std::endl;

            for (int32_t cave = 0; cave < caveCount; ++cave) {
                double x = static_cast<double>(sourcePos.getBlockX(legacyRandom.nextInt(16)));
                double y = static_cast<double>(caveConfig.y->sample(legacyRandom, carvingContext));
                double z = static_cast<double>(sourcePos.getBlockZ(legacyRandom.nextInt(16)));

                double horizontalRadiusMultiplier = static_cast<double>(caveConfig.horizontalRadiusMultiplier->sample(legacyRandom));
                double verticalRadiusMultiplier = static_cast<double>(caveConfig.verticalRadiusMultiplier->sample(legacyRandom));
                double floorLevel = static_cast<double>(caveConfig.floorLevel->sample(legacyRandom));

                g_traceFile << "\nCave " << cave << ": start=(" << x << "," << y << "," << z << ")"
                            << " hMult=" << horizontalRadiusMultiplier
                            << " vMult=" << verticalRadiusMultiplier
                            << " floor=" << floorLevel << std::endl;

                int32_t tunnels = 1;

                // Room check
                if (legacyRandom.nextInt(4) == 0) {
                    double yScale = static_cast<double>(caveConfig.yScale->sample(legacyRandom));
                    float thickness = 1.0f + legacyRandom.nextFloat() * 6.0f;

                    // Room ellipsoid
                    double angle = static_cast<double>(static_cast<float>(M_PI) / 2.0f);
                    double horizontalRadius = static_cast<double>(1.5f) + static_cast<double>(Mth::sin(angle) * thickness);
                    double verticalRadius = horizontalRadius * yScale;

                    g_traceFile << "  Room: thickness=" << thickness << " yScale=" << yScale << std::endl;

                    traceCarveEllipsoid(carvingContext, caveConfig, chunk, aquifer,
                        x + 1.0, y, z, horizontalRadius, verticalRadius, mask, floorLevel, totalEllipsoids++);

                    tunnels += legacyRandom.nextInt(4);
                }

                // Tunnels
                for (int32_t i = 0; i < tunnels; ++i) {
                    float horizontalRotation = legacyRandom.nextFloat() * (static_cast<float>(M_PI) * 2.0f);
                    float verticalRotation = (legacyRandom.nextFloat() - 0.5f) / 4.0f;

                    // Thickness calculation
                    float r1 = legacyRandom.nextFloat();
                    float r2 = legacyRandom.nextFloat();
                    float thickness = r1 * 2.0f + r2;
                    if (legacyRandom.nextInt(10) == 0) {
                        float r3 = legacyRandom.nextFloat();
                        float r4 = legacyRandom.nextFloat();
                        thickness *= r3 * r4 * 3.0f + 1.0f;
                    }

                    int32_t distance = maxDistance - legacyRandom.nextInt(maxDistance / 4);
                    int64_t tunnelSeed = legacyRandom.nextLong();

                    g_traceFile << "  Tunnel " << i << ": seed=" << tunnelSeed
                                << " thickness=" << thickness << " dist=" << distance << std::endl;

                    // Trace the tunnel
                    LegacyRandomSource tunnelRandom(tunnelSeed);
                    int32_t splitPoint = tunnelRandom.nextInt(distance / 2) + distance / 4;
                    bool steep = tunnelRandom.nextInt(6) == 0;

                    double tx = x, ty = y, tz = z;
                    float yRota = 0.0f, xRota = 0.0f;

                    for (int32_t step = 0; step < distance; ++step) {
                        double stepAngle = static_cast<double>(static_cast<float>(M_PI) * static_cast<float>(step) / static_cast<float>(distance));
                        double horizontalRadius = static_cast<double>(1.5f) + static_cast<double>(Mth::sin(stepAngle) * thickness);
                        double verticalRadius = horizontalRadius * 1.0;  // yScale = 1.0

                        float cosX = Mth::cos(static_cast<double>(verticalRotation));
                        tx += static_cast<double>(Mth::cos(static_cast<double>(horizontalRotation)) * cosX);
                        ty += static_cast<double>(Mth::sin(static_cast<double>(verticalRotation)));
                        tz += static_cast<double>(Mth::sin(static_cast<double>(horizontalRotation)) * cosX);

                        verticalRotation *= steep ? 0.92f : 0.7f;
                        verticalRotation += xRota * 0.1f;
                        horizontalRotation += yRota * 0.1f;

                        xRota *= 0.9f;
                        yRota *= 0.75f;

                        float rx1 = tunnelRandom.nextFloat();
                        float rx2 = tunnelRandom.nextFloat();
                        float rx3 = tunnelRandom.nextFloat();
                        xRota += (rx1 - rx2) * rx3 * 2.0f;
                        float ry1 = tunnelRandom.nextFloat();
                        float ry2 = tunnelRandom.nextFloat();
                        float ry3 = tunnelRandom.nextFloat();
                        yRota += (ry1 - ry2) * ry3 * 4.0f;

                        // Check for split
                        if (step == splitPoint && thickness > 1.0f) {
                            g_traceFile << "    Split at step " << step << std::endl;
                            // Skip branch tracing for now
                            tunnelRandom.nextLong();
                            tunnelRandom.nextFloat();
                            tunnelRandom.nextLong();
                            tunnelRandom.nextFloat();
                            break;
                        }

                        // 75% chance to carve
                        if (tunnelRandom.nextInt(4) != 0) {
                            // Check if ellipsoid intersects target chunk and is near lava level
                            if (ty >= -60 && ty <= -50) {
                                traceCarveEllipsoid(carvingContext, caveConfig, chunk, aquifer,
                                    tx, ty, tz,
                                    horizontalRadius * horizontalRadiusMultiplier,
                                    verticalRadius * verticalRadiusMultiplier,
                                    mask, floorLevel, totalEllipsoids++);
                            }
                        }
                    }
                }
            }
        }
    }

    // Count final blocks
    int airCount = 0, lavaCount = 0, deepslateCount = 0;
    for (int x = 0; x < 16; x++) {
        for (int z = 0; z < 16; z++) {
            for (int y = -64; y < 320; y++) {
                core::BlockPos pos(x, y, z);
                ::BlockState* block = chunk->getBlockState(pos);
                if (block) {
                    if (block->isAir()) airCount++;
                    else if (block->is(::minecraft::world::level::block::Blocks::LAVA->defaultBlockState())) lavaCount++;
                    else if (block->is(::minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState())) deepslateCount++;
                }
            }
        }
    }

    g_traceFile << "\n========================================" << std::endl;
    g_traceFile << "Final Block Counts" << std::endl;
    g_traceFile << "========================================" << std::endl;
    g_traceFile << "Air: " << airCount << std::endl;
    g_traceFile << "Lava: " << lavaCount << std::endl;
    g_traceFile << "Deepslate: " << deepslateCount << std::endl;
    g_traceFile << "Total ellipsoids traced: " << totalEllipsoids << std::endl;

    std::cout << "Done! Output written to cpp_carver_trace.txt" << std::endl;
    std::cout << "Air: " << airCount << ", Lava: " << lavaCount << std::endl;

    g_traceFile.close();

    delete noiseChunk;
    delete chunk;
    delete fluidPicker;
    delete randomState;
    delete settings;

    return 0;
}
