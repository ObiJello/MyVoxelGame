#include "levelgen/carver/CanyonWorldCarver.h"
#include "math/Mth.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace minecraft {
namespace levelgen {
namespace carver {

bool CanyonWorldCarver::carve(
    CarvingContext& context,
    const CanyonCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    XoroshiroRandomSource& random,
    Aquifer* aquifer,
    const ::world::ChunkPos& sourceChunkPos,
    CarvingMask& mask
) {
    // Reference: CanyonWorldCarver.java lines 24-37
    int32_t maxDistance = (getRange() * 2 - 1) * 16;

    // Random starting position
    double x = static_cast<double>(sourceChunkPos.getBlockX(random.nextInt(16)));
    int32_t y = configuration.y->sample(random, context);
    double z = static_cast<double>(sourceChunkPos.getBlockZ(random.nextInt(16)));

    // Random rotation angles
    float horizontalRotation = random.nextFloat() * (static_cast<float>(M_PI) * 2.0f);
    float verticalRotation = configuration.verticalRotation->sample(random);

    // Scale and shape parameters
    double yScale = static_cast<double>(configuration.yScale->sample(random));
    float thickness = configuration.shape.thickness->sample(random);
    int32_t distance = static_cast<int32_t>(static_cast<float>(maxDistance) * configuration.shape.distanceFactor->sample(random));

    doCarve(context, configuration, chunk, biomeGetter, random.nextLong(), aquifer,
            x, static_cast<double>(y), z, thickness, horizontalRotation, verticalRotation,
            0, distance, yScale, mask);

    return true;
}

bool CanyonWorldCarver::carve(
    CarvingContext& context,
    const CanyonCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    LegacyRandomSource& random,
    Aquifer* aquifer,
    const ::world::ChunkPos& sourceChunkPos,
    CarvingMask& mask
) {
    // Reference: CanyonWorldCarver.java lines 24-37
    int32_t maxDistance = (getRange() * 2 - 1) * 16;

    // Random starting position
    double x = static_cast<double>(sourceChunkPos.getBlockX(random.nextInt(16)));
    int32_t y = configuration.y->sample(random, context);
    double z = static_cast<double>(sourceChunkPos.getBlockZ(random.nextInt(16)));

    // Random rotation angles
    float horizontalRotation = random.nextFloat() * (static_cast<float>(M_PI) * 2.0f);
    float verticalRotation = configuration.verticalRotation->sample(random);

    // Scale and shape parameters
    double yScale = static_cast<double>(configuration.yScale->sample(random));
    float thickness = configuration.shape.thickness->sample(random);
    int32_t distance = static_cast<int32_t>(static_cast<float>(maxDistance) * configuration.shape.distanceFactor->sample(random));

    doCarve(context, configuration, chunk, biomeGetter, random.nextLong(), aquifer,
            x, static_cast<double>(y), z, thickness, horizontalRotation, verticalRotation,
            0, distance, yScale, mask);

    return true;
}

void CanyonWorldCarver::doCarve(
    CarvingContext& context,
    const CanyonCarverConfiguration& configuration,
    ::world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    int64_t tunnelSeed,
    Aquifer* aquifer,
    double x, double y, double z,
    float thickness,
    float horizontalRotation,
    float verticalRotation,
    int32_t step,
    int32_t distance,
    double yScale,
    CarvingMask& mask
) {
    // Reference: CanyonWorldCarver.java lines 39-70
    // Java: RandomSource random = RandomSource.create(tunnelSeed);
    // Note: Java uses LegacyRandomSource for RandomSource.create()
    LegacyRandomSource random(tunnelSeed);

    // Initialize width factors for variable canyon width per height
    std::vector<float> widthFactorPerHeight = initWidthFactors(context, configuration, random);

    // Rotation deltas
    float yRota = 0.0f;
    float xRota = 0.0f;

    // Walk the canyon path
    for (int32_t currentStep = step; currentStep < distance; ++currentStep) {
        // Calculate base radius using sine curve - Reference: line 46
        // CRITICAL: Java uses float arithmetic for the angle calculation!
        // Java: (double)((float)currentStep * (float)Math.PI / (float)distance)
        double angle = static_cast<double>(static_cast<float>(currentStep) * static_cast<float>(M_PI) / static_cast<float>(distance));
        double horizontalRadius = static_cast<double>(1.5f) + static_cast<double>(Mth::sin(angle) * thickness);
        double verticalRadius = horizontalRadius * yScale;

        // Apply horizontal radius factor - Reference: line 48
        horizontalRadius *= static_cast<double>(configuration.shape.horizontalRadiusFactor->sample(random));

        // Update vertical radius based on position - Reference: line 49
        verticalRadius = updateVerticalRadius(configuration, random, verticalRadius,
                                               static_cast<float>(distance), static_cast<float>(currentStep));

        // Move along the canyon - Reference: lines 50-54
        // CRITICAL: Java does float multiplication first, then casts to double
        // Java: x += (double)(Mth.cos((double)horizontalRotation) * xc);
        float xc = Mth::cos(static_cast<double>(verticalRotation));
        float xs = Mth::sin(static_cast<double>(verticalRotation));
        x += static_cast<double>(Mth::cos(static_cast<double>(horizontalRotation)) * xc);
        y += static_cast<double>(xs);
        z += static_cast<double>(Mth::sin(static_cast<double>(horizontalRotation)) * xc);

        // Apply rotation damping (different from caves) - Reference: lines 55-61
        verticalRotation *= 0.7f;
        verticalRotation += xRota * 0.05f;
        horizontalRotation += yRota * 0.05f;
        xRota *= 0.8f;
        yRota *= 0.5f;
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

        // 75% chance to carve at this step - Reference: lines 62-68
        if (random.nextInt(4) != 0) {
            // Check if we can still reach the target chunk
            if (!canReach(chunk->getPos(), x, z, currentStep, distance, thickness)) {
                return;
            }

            // Create skip checker that captures widthFactorPerHeight
            CarveSkipChecker skipChecker = [this, &context, &widthFactorPerHeight](
                const CarvingContext& c, double xd, double yd, double zd, int32_t worldY
            ) {
                return this->shouldSkip(context, widthFactorPerHeight, xd, yd, zd, worldY);
            };

            // Carve the ellipsoid
            carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer,
                           x, y, z, horizontalRadius, verticalRadius, mask, skipChecker);
        }
    }
}

std::vector<float> CanyonWorldCarver::initWidthFactors(
    const CarvingContext& context,
    const CanyonCarverConfiguration& configuration,
    LegacyRandomSource& random
) {
    // Reference: CanyonWorldCarver.java lines 73-87
    int32_t depth = context.getGenDepth();
    std::vector<float> widthFactorPerHeight(depth);

    float widthFactor = 1.0f;

    for (int32_t yIndex = 0; yIndex < depth; ++yIndex) {
        // Periodically update width factor for smoothing
        if (yIndex == 0 || random.nextInt(configuration.shape.widthSmoothness) == 0) {
            // CRITICAL: Must evaluate random calls in order
            float r1 = random.nextFloat();
            float r2 = random.nextFloat();
            widthFactor = 1.0f + r1 * r2;
        }
        widthFactorPerHeight[yIndex] = widthFactor * widthFactor;
    }

    return widthFactorPerHeight;
}

double CanyonWorldCarver::updateVerticalRadius(
    const CanyonCarverConfiguration& configuration,
    LegacyRandomSource& random,
    double verticalRadius,
    float distance,
    float currentStep
) {
    // Reference: CanyonWorldCarver.java lines 89-93
    // Create a parabolic multiplier that peaks in the middle
    float verticalMultiplier = 1.0f - std::abs(0.5f - currentStep / distance) * 2.0f;
    float factor = configuration.shape.verticalRadiusDefaultFactor +
                   configuration.shape.verticalRadiusCenterFactor * verticalMultiplier;

    // Apply random variation between 0.75 and 1.0
    float randomFactor = 0.75f + random.nextFloat() * 0.25f;

    return factor * verticalRadius * static_cast<double>(randomFactor);
}

bool CanyonWorldCarver::shouldSkip(
    const CarvingContext& context,
    const std::vector<float>& widthFactorPerHeight,
    double xd, double yd, double zd,
    int32_t y
) {
    // Reference: CanyonWorldCarver.java lines 95-98
    int32_t yIndex = y - context.getMinGenY();

    // Bounds check
    if (yIndex < 1 || yIndex >= static_cast<int32_t>(widthFactorPerHeight.size())) {
        return true;
    }

    // Canyon uses modified ellipsoid equation with per-height width factor
    // and fixed 6.0 divisor for vertical component
    return (xd * xd + zd * zd) * static_cast<double>(widthFactorPerHeight[yIndex - 1]) +
           yd * yd / 6.0 >= 1.0;
}

} // namespace carver
} // namespace levelgen
} // namespace minecraft
