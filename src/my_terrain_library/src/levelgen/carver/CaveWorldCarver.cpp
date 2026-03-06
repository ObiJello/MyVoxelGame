#include "levelgen/carver/CaveWorldCarver.h"
#include "core/SectionPos.h"
#include "random/LegacyRandomSource.h"
#include "math/Mth.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace minecraft {
namespace levelgen {
namespace carver {

bool CaveWorldCarver::carve(
    CarvingContext& context,
    const CaveCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    XoroshiroRandomSource& random,
    Aquifer* aquifer,
    const ::world::ChunkPos& sourceChunkPos,
    CarvingMask& mask
) {
    // Calculate max distance in blocks - Reference: line 26
    int32_t maxDistance = core::SectionPos::sectionToBlockCoord(getRange() * 2 - 1);

    // Determine number of caves - Reference: line 27
    // Triple nested random for variance
    int32_t caveCount = random.nextInt(random.nextInt(random.nextInt(getCaveBound()) + 1) + 1);

    for (int32_t cave = 0; cave < caveCount; ++cave) {
        // Random starting position - Reference: lines 30-32
        double x = static_cast<double>(sourceChunkPos.getBlockX(random.nextInt(16)));
        double y = static_cast<double>(configuration.y->sample(random, context));
        double z = static_cast<double>(sourceChunkPos.getBlockZ(random.nextInt(16)));

        // Sample radius multipliers - Reference: lines 33-35
        double horizontalRadiusMultiplier = static_cast<double>(configuration.horizontalRadiusMultiplier->sample(random));
        double verticalRadiusMultiplier = static_cast<double>(configuration.verticalRadiusMultiplier->sample(random));
        double floorLevel = static_cast<double>(configuration.floorLevel->sample(random));

        // Create skip checker with captured floor level - Reference: line 36
        CarveSkipChecker skipChecker = [floorLevel](const CarvingContext& c, double xd, double yd, double zd, int32_t worldY) {
            return shouldSkip(xd, yd, zd, floorLevel);
        };

        int32_t tunnels = 1;

        // 25% chance to create a room first - Reference: lines 38-43
        if (random.nextInt(4) == 0) {
            double yScale = static_cast<double>(configuration.yScale->sample(random));
            float thickness = 1.0f + random.nextFloat() * 6.0f;
            createRoom(context, configuration, chunk, biomeGetter, aquifer, x, y, z,
                       thickness, yScale, mask, skipChecker);
            tunnels += random.nextInt(4);
        }

        // Create tunnels - Reference: lines 45-52
        for (int32_t i = 0; i < tunnels; ++i) {
            float horizontalRotation = random.nextFloat() * (static_cast<float>(M_PI) * 2.0f);
            float verticalRotation = (random.nextFloat() - 0.5f) / 4.0f;
            float thickness = getThickness(random);
            int32_t distance = maxDistance - random.nextInt(maxDistance / 4);

            createTunnel(context, configuration, chunk, biomeGetter, random.nextLong(), aquifer,
                         x, y, z, horizontalRadiusMultiplier, verticalRadiusMultiplier,
                         thickness, horizontalRotation, verticalRotation,
                         0, distance, getYScale(), mask, skipChecker);
        }
    }

    return true;
}

float CaveWorldCarver::getThickness(XoroshiroRandomSource& random) const {
    // Reference: CaveWorldCarver.java lines 62-68
    // CRITICAL: Must evaluate random calls in order (C++ expression order is undefined)
    float r1 = random.nextFloat();
    float r2 = random.nextFloat();
    float thickness = r1 * 2.0f + r2;

    // 10% chance for extra thick tunnel
    if (random.nextInt(10) == 0) {
        float r3 = random.nextFloat();
        float r4 = random.nextFloat();
        thickness *= r3 * r4 * 3.0f + 1.0f;
    }

    return thickness;
}

void CaveWorldCarver::createRoom(
    CarvingContext& context,
    const CaveCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    Aquifer* aquifer,
    double x, double y, double z,
    float thickness,
    double yScale,
    CarvingMask& mask,
    CarveSkipChecker skipChecker
) {
    // Reference: CaveWorldCarver.java lines 75-79
    // CRITICAL: Java uses float arithmetic for the angle calculation!
    // Java: (double)((float)Math.PI / 2F)
    double angle = static_cast<double>(static_cast<float>(M_PI) / 2.0f);
    double horizontalRadius = static_cast<double>(1.5f) + static_cast<double>(Mth::sin(angle) * thickness);
    double verticalRadius = horizontalRadius * yScale;

    // Offset x by 1.0 for room placement
    carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer,
                   x + 1.0, y, z, horizontalRadius, verticalRadius, mask, skipChecker);
}

void CaveWorldCarver::createTunnel(
    CarvingContext& context,
    const CaveCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    int64_t tunnelSeed,
    Aquifer* aquifer,
    double x, double y, double z,
    double horizontalRadiusMultiplier,
    double verticalRadiusMultiplier,
    float thickness,
    float horizontalRotation,
    float verticalRotation,
    int32_t step,
    int32_t dist,
    double yScale,
    CarvingMask& mask,
    CarveSkipChecker skipChecker
) {
    // Create random source for this tunnel - Reference: line 82
    XoroshiroRandomSource random(tunnelSeed);

    // Determine split point for branching - Reference: line 83
    int32_t splitPoint = random.nextInt(dist / 2) + dist / 4;

    // Whether this tunnel is steep (affects damping) - Reference: line 84
    bool steep = random.nextInt(6) == 0;

    // Rotation deltas - Reference: lines 85-86
    float yRota = 0.0f;
    float xRota = 0.0f;

    // Walk the tunnel - Reference: lines 88-115
    for (int32_t currentStep = step; currentStep < dist; ++currentStep) {
        // Calculate radius using sine curve - Reference: line 89
        // CRITICAL: Java uses float arithmetic for the angle calculation!
        // Java: (double)((float)Math.PI * (float)currentStep / (float)dist)
        double angle = static_cast<double>(static_cast<float>(M_PI) * static_cast<float>(currentStep) / static_cast<float>(dist));
        double horizontalRadius = static_cast<double>(1.5f) + static_cast<double>(Mth::sin(angle) * thickness);
        double verticalRadius = horizontalRadius * yScale;

        // Move along the tunnel - Reference: lines 91-94
        // CRITICAL: Java does float multiplication first, then casts to double
        // Java: x += (double)(Mth.cos((double)horizontalRotation) * cosX);
        float cosX = Mth::cos(static_cast<double>(verticalRotation));
        x += static_cast<double>(Mth::cos(static_cast<double>(horizontalRotation)) * cosX);
        y += static_cast<double>(Mth::sin(static_cast<double>(verticalRotation)));
        z += static_cast<double>(Mth::sin(static_cast<double>(horizontalRotation)) * cosX);

        // Apply rotation damping - Reference: lines 95-97
        verticalRotation *= steep ? 0.92f : 0.7f;
        verticalRotation += xRota * 0.1f;
        horizontalRotation += yRota * 0.1f;

        // Decay rotation deltas - Reference: lines 98-99
        xRota *= 0.9f;
        yRota *= 0.75f;

        // Add random variation - Reference: lines 100-101
        // CRITICAL: Must evaluate random calls in order (C++ expression order is undefined)
        {
            float rx1 = random.nextFloat();
            float rx2 = random.nextFloat();
            float rx3 = random.nextFloat();
            xRota += (rx1 - rx2) * rx3 * 2.0f;
            float ry1 = random.nextFloat();
            float ry2 = random.nextFloat();
            float ry3 = random.nextFloat();
            yRota += (ry1 - ry2) * ry3 * 4.0f;
        }

        // Check for tunnel split - Reference: lines 102-106
        if (currentStep == splitPoint && thickness > 1.0f) {
            // Branch into two tunnels
            // CRITICAL: Must evaluate random values in order (C++ argument order is undefined)
            // Java evaluates left-to-right: nextLong() then nextFloat() for each branch
            int64_t seed1 = random.nextLong();
            float thickness1 = random.nextFloat() * 0.5f + 0.5f;
            int64_t seed2 = random.nextLong();
            float thickness2 = random.nextFloat() * 0.5f + 0.5f;

            createTunnel(context, configuration, chunk, biomeGetter, seed1, aquifer,
                         x, y, z, horizontalRadiusMultiplier, verticalRadiusMultiplier,
                         thickness1,
                         horizontalRotation - static_cast<float>(M_PI) / 2.0f,
                         verticalRotation / 3.0f,
                         currentStep, dist, 1.0, mask, skipChecker);

            createTunnel(context, configuration, chunk, biomeGetter, seed2, aquifer,
                         x, y, z, horizontalRadiusMultiplier, verticalRadiusMultiplier,
                         thickness2,
                         horizontalRotation + static_cast<float>(M_PI) / 2.0f,
                         verticalRotation / 3.0f,
                         currentStep, dist, 1.0, mask, skipChecker);
            return;
        }

        // 75% chance to carve at this step - Reference: lines 108-114
        if (random.nextInt(4) != 0) {
            // Check if we can still reach the target chunk
            if (!canReach(chunk->getPos(), x, z, currentStep, dist, thickness)) {
                return;
            }

            // Carve the ellipsoid
            carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer,
                           x, y, z,
                           horizontalRadius * horizontalRadiusMultiplier,
                           verticalRadius * verticalRadiusMultiplier,
                           mask, skipChecker);
        }
    }
}

bool CaveWorldCarver::shouldSkip(double xd, double yd, double zd, double floorLevel) {
    // Reference: CaveWorldCarver.java lines 119-124
    // Skip if below floor level
    if (yd <= floorLevel) {
        return true;
    }
    // Skip if outside unit sphere
    return xd * xd + yd * yd + zd * zd >= 1.0;
}

//=============================================================================
// LegacyRandomSource versions for Java parity
//=============================================================================

bool CaveWorldCarver::carve(
    CarvingContext& context,
    const CaveCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    LegacyRandomSource& random,
    Aquifer* aquifer,
    const ::world::ChunkPos& sourceChunkPos,
    CarvingMask& mask
) {
    // Calculate max distance in blocks - Reference: line 26
    int32_t maxDistance = core::SectionPos::sectionToBlockCoord(getRange() * 2 - 1);

    // Determine number of caves - Reference: line 27
    // Triple nested random for variance
    int32_t caveCount = random.nextInt(random.nextInt(random.nextInt(getCaveBound()) + 1) + 1);

    for (int32_t cave = 0; cave < caveCount; ++cave) {
        // Random starting position - Reference: lines 30-32
        double x = static_cast<double>(sourceChunkPos.getBlockX(random.nextInt(16)));
        double y = static_cast<double>(configuration.y->sample(random, context));
        double z = static_cast<double>(sourceChunkPos.getBlockZ(random.nextInt(16)));

        // Sample radius multipliers - Reference: lines 33-35
        double horizontalRadiusMultiplier = static_cast<double>(configuration.horizontalRadiusMultiplier->sample(random));
        double verticalRadiusMultiplier = static_cast<double>(configuration.verticalRadiusMultiplier->sample(random));
        double floorLevel = static_cast<double>(configuration.floorLevel->sample(random));

        // Create skip checker with captured floor level - Reference: line 36
        CarveSkipChecker skipChecker = [floorLevel](const CarvingContext& c, double xd, double yd, double zd, int32_t worldY) {
            return shouldSkip(xd, yd, zd, floorLevel);
        };

        int32_t tunnels = 1;

        // 25% chance to create a room first - Reference: lines 38-43
        if (random.nextInt(4) == 0) {
            double yScale = static_cast<double>(configuration.yScale->sample(random));
            float thickness = 1.0f + random.nextFloat() * 6.0f;
            createRoom(context, configuration, chunk, biomeGetter, aquifer, x, y, z,
                       thickness, yScale, mask, skipChecker);
            tunnels += random.nextInt(4);
        }

        // Create tunnels - Reference: lines 45-52
        for (int32_t i = 0; i < tunnels; ++i) {
            float horizontalRotation = random.nextFloat() * (static_cast<float>(M_PI) * 2.0f);
            float verticalRotation = (random.nextFloat() - 0.5f) / 4.0f;
            float thickness = getThickness(random);
            int32_t distance = maxDistance - random.nextInt(maxDistance / 4);

            createTunnelLegacy(context, configuration, chunk, biomeGetter, random.nextLong(), aquifer,
                         x, y, z, horizontalRadiusMultiplier, verticalRadiusMultiplier,
                         thickness, horizontalRotation, verticalRotation,
                         0, distance, getYScale(), mask, skipChecker);
        }
    }

    return true;
}

float CaveWorldCarver::getThickness(LegacyRandomSource& random) const {
    // Reference: CaveWorldCarver.java lines 62-68
    // CRITICAL: Must evaluate random calls in order (C++ expression order is undefined)
    float r1 = random.nextFloat();
    float r2 = random.nextFloat();
    float thickness = r1 * 2.0f + r2;

    // 10% chance for extra thick tunnel
    if (random.nextInt(10) == 0) {
        float r3 = random.nextFloat();
        float r4 = random.nextFloat();
        thickness *= r3 * r4 * 3.0f + 1.0f;
    }

    return thickness;
}

void CaveWorldCarver::createTunnelLegacy(
    CarvingContext& context,
    const CaveCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    int64_t tunnelSeed,
    Aquifer* aquifer,
    double x, double y, double z,
    double horizontalRadiusMultiplier,
    double verticalRadiusMultiplier,
    float thickness,
    float horizontalRotation,
    float verticalRotation,
    int32_t step,
    int32_t dist,
    double yScale,
    CarvingMask& mask,
    CarveSkipChecker skipChecker
) {
    // Create LegacyRandomSource for this tunnel - Reference: line 82
    LegacyRandomSource random(tunnelSeed);

    // Determine split point for branching - Reference: line 83
    int32_t splitPoint = random.nextInt(dist / 2) + dist / 4;

    // Whether this tunnel is steep (affects damping) - Reference: line 84
    bool steep = random.nextInt(6) == 0;

    // Rotation deltas - Reference: lines 85-86
    float yRota = 0.0f;
    float xRota = 0.0f;

    // Walk the tunnel - Reference: lines 88-115
    for (int32_t currentStep = step; currentStep < dist; ++currentStep) {
        // Calculate radius using sine curve - Reference: line 89
        // CRITICAL: Java uses float arithmetic for the angle calculation!
        // Java: (double)((float)Math.PI * (float)currentStep / (float)dist)
        double angle = static_cast<double>(static_cast<float>(M_PI) * static_cast<float>(currentStep) / static_cast<float>(dist));
        double horizontalRadius = static_cast<double>(1.5f) + static_cast<double>(Mth::sin(angle) * thickness);
        double verticalRadius = horizontalRadius * yScale;

        // Move along the tunnel - Reference: lines 91-94
        // CRITICAL: Java does float multiplication first, then casts to double
        // Java: x += (double)(Mth.cos((double)horizontalRotation) * cosX);
        float cosX = Mth::cos(static_cast<double>(verticalRotation));
        x += static_cast<double>(Mth::cos(static_cast<double>(horizontalRotation)) * cosX);
        y += static_cast<double>(Mth::sin(static_cast<double>(verticalRotation)));
        z += static_cast<double>(Mth::sin(static_cast<double>(horizontalRotation)) * cosX);

        // Apply rotation damping - Reference: lines 95-97
        verticalRotation *= steep ? 0.92f : 0.7f;
        verticalRotation += xRota * 0.1f;
        horizontalRotation += yRota * 0.1f;

        // Decay rotation deltas - Reference: lines 98-99
        xRota *= 0.9f;
        yRota *= 0.75f;

        // Add random variation - Reference: lines 100-101
        float r1 = random.nextFloat();
        float r2 = random.nextFloat();
        float r3 = random.nextFloat();
        xRota += (r1 - r2) * r3 * 2.0f;
        float r4 = random.nextFloat();
        float r5 = random.nextFloat();
        float r6 = random.nextFloat();
        yRota += (r4 - r5) * r6 * 4.0f;

        // Check for tunnel split - Reference: lines 102-106
        if (currentStep == splitPoint && thickness > 1.0f) {
            // Branch into two tunnels
            // CRITICAL: Must evaluate random values in order (C++ argument order is undefined)
            // Java evaluates left-to-right: nextLong() then nextFloat() for each branch
            int64_t seed1 = random.nextLong();
            float thickness1 = random.nextFloat() * 0.5f + 0.5f;
            int64_t seed2 = random.nextLong();
            float thickness2 = random.nextFloat() * 0.5f + 0.5f;

            createTunnelLegacy(context, configuration, chunk, biomeGetter, seed1, aquifer,
                         x, y, z, horizontalRadiusMultiplier, verticalRadiusMultiplier,
                         thickness1,
                         horizontalRotation - static_cast<float>(M_PI) / 2.0f,
                         verticalRotation / 3.0f,
                         currentStep, dist, 1.0, mask, skipChecker);

            createTunnelLegacy(context, configuration, chunk, biomeGetter, seed2, aquifer,
                         x, y, z, horizontalRadiusMultiplier, verticalRadiusMultiplier,
                         thickness2,
                         horizontalRotation + static_cast<float>(M_PI) / 2.0f,
                         verticalRotation / 3.0f,
                         currentStep, dist, 1.0, mask, skipChecker);
            return;
        }

        // 75% chance to carve at this step - Reference: lines 108-114
        if (random.nextInt(4) != 0) {
            // Check if we can still reach the target chunk
            if (!canReach(chunk->getPos(), x, z, currentStep, dist, thickness)) {
                return;
            }

            // Carve the ellipsoid
            carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer,
                           x, y, z,
                           horizontalRadius * horizontalRadiusMultiplier,
                           verticalRadius * verticalRadiusMultiplier,
                           mask, skipChecker);
        }

    }
}

} // namespace carver
} // namespace levelgen
} // namespace minecraft
