#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include <cmath>
#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/feature/foliageplacers/*.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace foliageplacers {

// ============================================================================
// FoliagePlacer base class
// ============================================================================

void FoliagePlacer::placeLeavesRow(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    const core::BlockPos& origin,
    int currentRadius,
    int y,
    bool doubleTrunk
) {
    // Reference: FoliagePlacer.java placeLeavesRow() lines 66-79
    int offset = doubleTrunk ? 1 : 0;

    for (int dx = -currentRadius; dx <= currentRadius + offset; ++dx) {
        for (int dz = -currentRadius; dz <= currentRadius + offset; ++dz) {
            if (!shouldSkipLocationSigned(random, dx, y, dz, currentRadius, doubleTrunk)) {
                core::BlockPos pos = origin.offset(dx, y, dz);
                tryPlaceLeaf(foliageSetter, random, foliageProvider, pos);
            }
        }
    }
}

bool FoliagePlacer::shouldSkipLocationSigned(
    XoroshiroRandomSource& random,
    int dx, int y, int dz,
    int currentRadius,
    bool doubleTrunk
) const {
    // Reference: FoliagePlacer.java shouldSkipLocationSigned() lines 52-64
    int minDx;
    int minDz;
    if (doubleTrunk) {
        minDx = std::min(std::abs(dx), std::abs(dx - 1));
        minDz = std::min(std::abs(dz), std::abs(dz - 1));
    } else {
        minDx = std::abs(dx);
        minDz = std::abs(dz);
    }

    return shouldSkipLocation(random, minDx, y, minDz, currentRadius, doubleTrunk);
}

void FoliagePlacer::tryPlaceLeaf(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    const core::BlockPos& pos
) {
    // Reference: FoliagePlacer.java tryPlaceLeaf() lines 117-130
    // Simplified - in full implementation would check for persistent property
    // and handle waterlogged state
    BlockState* foliageState = foliageProvider->getState(random, pos);
    foliageSetter.set(pos, foliageState);
}

void FoliagePlacer::placeLeavesRowWithHangingLeavesBelow(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    const core::BlockPos& origin,
    int currentRadius,
    int y,
    bool doubleTrunk,
    float hangingLeavesChance,
    float hangingLeavesExtensionChance
) {
    // Reference: FoliagePlacer.java placeLeavesRowWithHangingLeavesBelow()
    // First place normal leaves row
    placeLeavesRow(foliageSetter, random, foliageProvider, origin, currentRadius, y, doubleTrunk);

    // Then place hanging leaves below
    int offset = doubleTrunk ? 1 : 0;
    for (int dx = -currentRadius; dx <= currentRadius + offset; ++dx) {
        for (int dz = -currentRadius; dz <= currentRadius + offset; ++dz) {
            if (!shouldSkipLocationSigned(random, dx, y, dz, currentRadius, doubleTrunk)) {
                if (random.nextFloat() < hangingLeavesChance) {
                    core::BlockPos pos = origin.offset(dx, y - 1, dz);
                    tryPlaceLeaf(foliageSetter, random, foliageProvider, pos);

                    // Extension below
                    if (random.nextFloat() < hangingLeavesExtensionChance) {
                        core::BlockPos posBelow = origin.offset(dx, y - 2, dz);
                        tryPlaceLeaf(foliageSetter, random, foliageProvider, posBelow);
                    }
                }
            }
        }
    }
}

// ============================================================================
// BlobFoliagePlacer
// Reference: BlobFoliagePlacer.java
// ============================================================================

void BlobFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: BlobFoliagePlacer.java createFoliage() lines 29-35
    for (int yo = offset; yo >= offset - foliageHeight; --yo) {
        int currentRadius = std::max(leafRadius + attachment.radiusOffset() - 1 - yo / 2, 0);
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      attachment.pos(), currentRadius, yo, attachment.doubleTrunk());
    }
}

// ============================================================================
// SpruceFoliagePlacer
// Reference: SpruceFoliagePlacer.java
// ============================================================================

void SpruceFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: SpruceFoliagePlacer.java createFoliage() lines 24-40
    const core::BlockPos& foliagePos = attachment.pos();
    int currentRadius = random.nextInt(2);
    int maxRadius = 1;
    int minRadius = 0;

    for (int yo = offset; yo >= -foliageHeight; --yo) {
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      foliagePos, currentRadius, yo, attachment.doubleTrunk());
        if (currentRadius >= maxRadius) {
            currentRadius = minRadius;
            minRadius = 1;
            maxRadius = std::min(maxRadius + 1, leafRadius + attachment.radiusOffset());
        } else {
            ++currentRadius;
        }
    }
}

// ============================================================================
// PineFoliagePlacer
// Reference: PineFoliagePlacer.java
// ============================================================================

void PineFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: PineFoliagePlacer.java createFoliage() lines 23-35
    int currentRadius = 0;

    for (int yo = offset; yo >= offset - foliageHeight; --yo) {
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      attachment.pos(), currentRadius, yo, attachment.doubleTrunk());
        if (currentRadius >= 1 && yo == offset - foliageHeight + 1) {
            --currentRadius;
        } else if (currentRadius < leafRadius + attachment.radiusOffset()) {
            ++currentRadius;
        }
    }
}

int PineFoliagePlacer::foliageRadius(XoroshiroRandomSource& random, int trunkHeight) const {
    // Reference: PineFoliagePlacer.java foliageRadius() lines 37-39
    return FoliagePlacer::foliageRadius(random, trunkHeight) +
           random.nextInt(std::max(trunkHeight + 1, 1));
}

// ============================================================================
// AcaciaFoliagePlacer
// Reference: AcaciaFoliagePlacer.java
// ============================================================================

void AcaciaFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: AcaciaFoliagePlacer.java createFoliage() lines 22-28
    bool doubleTrunk = attachment.doubleTrunk();
    core::BlockPos foliagePos = attachment.pos().above(offset);

    placeLeavesRow(foliageSetter, random, foliageProvider,
                  foliagePos, leafRadius + attachment.radiusOffset(), -1 - foliageHeight, doubleTrunk);
    placeLeavesRow(foliageSetter, random, foliageProvider,
                  foliagePos, leafRadius - 1, -foliageHeight, doubleTrunk);
    placeLeavesRow(foliageSetter, random, foliageProvider,
                  foliagePos, leafRadius + attachment.radiusOffset() - 1, 0, doubleTrunk);
}

// ============================================================================
// DarkOakFoliagePlacer
// Reference: DarkOakFoliagePlacer.java
// ============================================================================

void DarkOakFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: DarkOakFoliagePlacer.java createFoliage() lines 22-37
    core::BlockPos pos = attachment.pos().above(offset);
    bool doubleTrunk = attachment.doubleTrunk();

    if (doubleTrunk) {
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      pos, leafRadius + 2, -1, doubleTrunk);
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      pos, leafRadius + 3, 0, doubleTrunk);
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      pos, leafRadius + 2, 1, doubleTrunk);
        if (random.nextBoolean()) {
            placeLeavesRow(foliageSetter, random, foliageProvider,
                          pos, leafRadius, 2, doubleTrunk);
        }
    } else {
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      pos, leafRadius + 2, -1, doubleTrunk);
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      pos, leafRadius + 1, 0, doubleTrunk);
    }
}

bool DarkOakFoliagePlacer::shouldSkipLocationSigned(
    XoroshiroRandomSource& random,
    int dx, int y, int dz,
    int currentRadius,
    bool doubleTrunk
) const {
    // Reference: DarkOakFoliagePlacer.java shouldSkipLocationSigned() lines 43-45
    if (y != 0 || !doubleTrunk ||
        (dx != -currentRadius && dx < currentRadius) ||
        (dz != -currentRadius && dz < currentRadius)) {
        return FoliagePlacer::shouldSkipLocationSigned(random, dx, y, dz, currentRadius, doubleTrunk);
    }
    return true;
}

bool DarkOakFoliagePlacer::shouldSkipLocation(
    XoroshiroRandomSource& random,
    int dx, int y, int dz,
    int currentRadius,
    bool doubleTrunk
) const {
    // Reference: DarkOakFoliagePlacer.java shouldSkipLocation() lines 47-55
    if (y == -1 && !doubleTrunk) {
        return dx == currentRadius && dz == currentRadius;
    } else if (y == 1) {
        return dx + dz > currentRadius * 2 - 2;
    }
    return false;
}

// ============================================================================
// MegaJungleFoliagePlacer
// Reference: MegaJungleFoliagePlacer.java
// ============================================================================

void MegaJungleFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: MegaJungleFoliagePlacer.java createFoliage() lines 24-32
    int leafHeight = attachment.doubleTrunk() ? foliageHeight : 1 + random.nextInt(2);

    for (int yo = offset; yo >= offset - leafHeight; --yo) {
        int currentRadius = leafRadius + attachment.radiusOffset() + 1 - yo;
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      attachment.pos(), currentRadius, yo, attachment.doubleTrunk());
    }
}

// ============================================================================
// MegaPineFoliagePlacer
// Reference: MegaPineFoliagePlacer.java
// ============================================================================

void MegaPineFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: MegaPineFoliagePlacer.java createFoliage() lines 25-43
    const core::BlockPos& foliagePos = attachment.pos();
    int prevRadius = 0;

    for (int yy = foliagePos.getY() - foliageHeight + offset; yy <= foliagePos.getY() + offset; ++yy) {
        int yo = foliagePos.getY() - yy;
        // Reference: line 31 - calculate smooth radius with 3.5F factor
        int smoothRadius = leafRadius + attachment.radiusOffset() +
                          static_cast<int>(std::floor(static_cast<float>(yo) / static_cast<float>(foliageHeight) * 3.5f));

        int jaggedRadius;
        // Reference: lines 33-37 - add jaggedness to even rows
        if (yo > 0 && smoothRadius == prevRadius && (yy & 1) == 0) {
            jaggedRadius = smoothRadius + 1;
        } else {
            jaggedRadius = smoothRadius;
        }

        // Reference: line 39
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      core::BlockPos(foliagePos.getX(), yy, foliagePos.getZ()),
                      jaggedRadius, 0, attachment.doubleTrunk());
        prevRadius = smoothRadius;
    }
}

// ============================================================================
// RandomSpreadFoliagePlacer
// Reference: RandomSpreadFoliagePlacer.java
// ============================================================================

void RandomSpreadFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: RandomSpreadFoliagePlacer.java createFoliage() lines 27-36
    const core::BlockPos& origin = attachment.pos();
    core::BlockPos::MutableBlockPos pos;

    for (int i = 0; i < m_leafPlacementAttempts; ++i) {
        // Reference: line 32 - calculate random offset
        pos.setWithOffset(origin,
            random.nextInt(leafRadius) - random.nextInt(leafRadius),
            random.nextInt(foliageHeight) - random.nextInt(foliageHeight),
            random.nextInt(leafRadius) - random.nextInt(leafRadius));
        tryPlaceLeaf(foliageSetter, random, foliageProvider, pos);
    }
}

// ============================================================================
// CherryFoliagePlacer
// Reference: CherryFoliagePlacer.java
// ============================================================================

void CherryFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    XoroshiroRandomSource& random,
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    // Reference: CherryFoliagePlacer.java createFoliage() lines 33-46
    bool doubleTrunk = attachment.doubleTrunk();
    core::BlockPos foliagePos = attachment.pos().above(offset);
    int currentRadius = leafRadius + attachment.radiusOffset() - 1;

    // Reference: lines 37-38 - place top layers
    placeLeavesRow(foliageSetter, random, foliageProvider,
                  foliagePos, currentRadius - 2, foliageHeight - 3, doubleTrunk);
    placeLeavesRow(foliageSetter, random, foliageProvider,
                  foliagePos, currentRadius - 1, foliageHeight - 4, doubleTrunk);

    // Reference: lines 40-42 - place main layers
    for (int y = foliageHeight - 5; y >= 0; --y) {
        placeLeavesRow(foliageSetter, random, foliageProvider,
                      foliagePos, currentRadius, y, doubleTrunk);
    }

    // Reference: lines 44-45 - place bottom layers with hanging leaves
    placeLeavesRowWithHangingLeavesBelow(foliageSetter, random, foliageProvider,
                                         foliagePos, currentRadius, -1, doubleTrunk,
                                         m_hangingLeavesChance, m_hangingLeavesExtensionChance);
    placeLeavesRowWithHangingLeavesBelow(foliageSetter, random, foliageProvider,
                                         foliagePos, currentRadius - 1, -2, doubleTrunk,
                                         m_hangingLeavesChance, m_hangingLeavesExtensionChance);
}

bool CherryFoliagePlacer::shouldSkipLocation(
    XoroshiroRandomSource& random,
    int dx, int y, int dz,
    int currentRadius,
    bool doubleTrunk
) const {
    // Reference: CherryFoliagePlacer.java shouldSkipLocation() lines 52-64
    // Reference: line 53 - bottom layer hole chance
    if (y == -1 && (dx == currentRadius || dz == currentRadius) &&
        random.nextFloat() < m_wideBottomLayerHoleChance) {
        return true;
    }

    // Reference: lines 56-63
    bool corner = dx == currentRadius && dz == currentRadius;
    bool wideLayer = currentRadius > 2;

    if (wideLayer) {
        // Reference: line 59 - wider layers have diagonal cutoffs
        return corner || (dx + dz > currentRadius * 2 - 2 && random.nextFloat() < m_cornerHoleChance);
    } else {
        // Reference: line 61 - narrow layers only skip corners randomly
        return corner && random.nextFloat() < m_cornerHoleChance;
    }
}

} // namespace foliageplacers
} // namespace feature
} // namespace levelgen
} // namespace minecraft
