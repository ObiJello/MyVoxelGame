#pragma once

#include "core/BlockPos.h"
#include "world/level/block/state/BlockState.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/carver/CarverConfiguration.h"
#include <vector>
#include <memory>
#include <functional>

// Reference: net/minecraft/world/level/levelgen/feature/foliageplacers/FoliagePlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/foliageplacers/BlobFoliagePlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/foliageplacers/SpruceFoliagePlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/foliageplacers/PineFoliagePlacer.java

namespace minecraft {

// Forward declarations
namespace levelgen::feature::stateproviders { class BlockStateProvider; }

namespace levelgen {
namespace feature {
namespace foliageplacers {

// Forward declaration for TreeConfiguration
class TreeConfiguration;

/**
 * FoliageSetter - Interface for setting foliage blocks
 * Reference: FoliagePlacer.FoliageSetter
 */
class FoliageSetter {
public:
    virtual ~FoliageSetter() = default;

    virtual void set(const core::BlockPos& pos, BlockState* blockState) = 0;
    virtual bool isSet(const core::BlockPos& pos) const = 0;

    /**
     * Check if a leaf can be placed at the given position
     * Reference: FoliagePlacer.java tryPlaceLeaf() calls validTreePos() before placing
     * Default returns true for backward compatibility
     */
    virtual bool canPlace(const core::BlockPos& pos) const { return true; }
};

/**
 * FoliageAttachment - Attachment point for foliage on trunk
 * Reference: FoliagePlacer.FoliageAttachment
 */
class FoliageAttachment {
private:
    core::BlockPos m_pos;
    int m_radiusOffset;
    bool m_doubleTrunk;

public:
    FoliageAttachment(const core::BlockPos& pos, int radiusOffset, bool doubleTrunk)
        : m_pos(pos)
        , m_radiusOffset(radiusOffset)
        , m_doubleTrunk(doubleTrunk)
    {}

    const core::BlockPos& pos() const { return m_pos; }
    int radiusOffset() const { return m_radiusOffset; }
    bool doubleTrunk() const { return m_doubleTrunk; }
};

/**
 * FoliagePlacer - Abstract base class for foliage placement
 * Reference: FoliagePlacer.java
 */
class FoliagePlacer {
protected:
    std::shared_ptr<carver::IntProvider> m_radius;
    std::shared_ptr<carver::IntProvider> m_offset;

public:
    FoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset
    )
        : m_radius(radius)
        , m_offset(offset)
    {}

    virtual ~FoliagePlacer() = default;

    /**
     * Create foliage at attachment point
     * Reference: FoliagePlacer.java lines 34-36
     */
    void createFoliage(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius
    ) {
        int offset = m_offset->sample(random);
        createFoliageImpl(foliageSetter, random, foliageProvider, treeHeight, attachment, foliageHeight, leafRadius, offset);
    }

    /**
     * Get foliage height
     * Reference: FoliagePlacer.java line 40
     */
    virtual int foliageHeight(WorldgenRandom& random, int treeHeight) const = 0;

    /**
     * Get foliage radius
     * Reference: FoliagePlacer.java lines 42-44
     */
    virtual int foliageRadius(WorldgenRandom& random, int trunkHeight) const {
        return m_radius->sample(random);
    }

protected:
    /**
     * Create foliage implementation
     * Reference: FoliagePlacer.java line 38
     */
    virtual void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) = 0;

    /**
     * Check if location should be skipped
     * Reference: FoliagePlacer.java line 50
     */
    virtual bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const = 0;

    /**
     * Check if location should be skipped (signed version)
     * Reference: FoliagePlacer.java shouldSkipLocationSigned() lines 52-64
     */
    virtual bool shouldSkipLocationSigned(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const;

    /**
     * Place a row of leaves
     * Reference: FoliagePlacer.java lines 66-79
     */
    void placeLeavesRow(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        const core::BlockPos& origin,
        int currentRadius,
        int y,
        bool doubleTrunk
    );

    /**
     * Place a row of leaves with hanging leaves below
     * Reference: FoliagePlacer.java placeLeavesRowWithHangingLeavesBelow()
     */
    void placeLeavesRowWithHangingLeavesBelow(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        const core::BlockPos& origin,
        int currentRadius,
        int y,
        bool doubleTrunk,
        float hangingLeavesChance,
        float hangingLeavesExtensionChance
    );

    /**
     * Try to place a leaf block
     * Reference: FoliagePlacer.java tryPlaceLeaf() lines 117-130
     */
    static bool tryPlaceLeaf(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        const core::BlockPos& pos
    );
};

/**
 * BlobFoliagePlacer - Standard spherical foliage
 * Reference: BlobFoliagePlacer.java
 */
class BlobFoliagePlacer : public FoliagePlacer {
private:
    int m_height;

public:
    BlobFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        int height
    )
        : FoliagePlacer(radius, offset)
        , m_height(height)
    {}

    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return m_height;
    }

protected:
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        // Reference: BlobFoliagePlacer.java shouldSkipLocation() line 42
        // Skip corners randomly (50%) or always at y==0
        return dx == currentRadius && dz == currentRadius && (random.nextInt(2) == 0 || y == 0);
    }
};

/**
 * SpruceFoliagePlacer - Conical spruce foliage
 * Reference: SpruceFoliagePlacer.java
 */
class SpruceFoliagePlacer : public FoliagePlacer {
private:
    std::shared_ptr<carver::IntProvider> m_trunkHeight;

public:
    SpruceFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        std::shared_ptr<carver::IntProvider> trunkHeight
    )
        : FoliagePlacer(radius, offset)
        , m_trunkHeight(trunkHeight)
    {}

    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return std::max(4, treeHeight - m_trunkHeight->sample(random));
    }

protected:
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        return dx == currentRadius && dz == currentRadius && currentRadius > 0;
    }
};

/**
 * PineFoliagePlacer - Layered pine foliage
 * Reference: PineFoliagePlacer.java
 */
class PineFoliagePlacer : public FoliagePlacer {
private:
    std::shared_ptr<carver::IntProvider> m_height;

public:
    PineFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        std::shared_ptr<carver::IntProvider> height
    )
        : FoliagePlacer(radius, offset)
        , m_height(height)
    {}

    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return m_height->sample(random);
    }

    int foliageRadius(WorldgenRandom& random, int trunkHeight) const override;

protected:
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        return dx == currentRadius && dz == currentRadius && random.nextInt(2) == 0;
    }
};

/**
 * FancyFoliagePlacer - Oak tree fancy foliage
 * Reference: FancyFoliagePlacer.java
 */
class FancyFoliagePlacer : public BlobFoliagePlacer {
public:
    FancyFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        int height
    )
        : BlobFoliagePlacer(radius, offset, height)
    {}

protected:
    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        // More rounded shape
        return dx * dx + dz * dz > currentRadius * currentRadius;
    }
};

/**
 * AcaciaFoliagePlacer - Flat acacia-style foliage
 * Reference: AcaciaFoliagePlacer.java
 */
class AcaciaFoliagePlacer : public FoliagePlacer {
public:
    AcaciaFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset
    )
        : FoliagePlacer(radius, offset)
    {}

    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return 0;
    }

protected:
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        if (dx == 0 && dz == 0) {
            return false;
        }
        return (dx == currentRadius || dz == currentRadius) && random.nextInt(2) == 0;
    }
};

/**
 * BushFoliagePlacer - Small bush foliage
 * Reference: BushFoliagePlacer.java
 */
class BushFoliagePlacer : public BlobFoliagePlacer {
public:
    BushFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        int height
    )
        : BlobFoliagePlacer(radius, offset, height)
    {}

protected:
    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        // More blocky
        return false;
    }
};

/**
 * DarkOakFoliagePlacer - Large dark oak foliage
 * Reference: DarkOakFoliagePlacer.java
 */
class DarkOakFoliagePlacer : public FoliagePlacer {
public:
    DarkOakFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset
    )
        : FoliagePlacer(radius, offset)
    {}

    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return 4;
    }

protected:
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocationSigned(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override;

    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override;
};

/**
 * MegaJungleFoliagePlacer - Large jungle tree foliage
 * Reference: MegaJungleFoliagePlacer.java
 */
class MegaJungleFoliagePlacer : public FoliagePlacer {
private:
    int m_height;

public:
    MegaJungleFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        int height
    )
        : FoliagePlacer(radius, offset)
        , m_height(height)
    {}

    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return m_height;
    }

protected:
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        return dx + dz >= 7;
    }
};

/**
 * MegaPineFoliagePlacer - Large pine tree foliage (mega spruce)
 * Reference: MegaPineFoliagePlacer.java
 */
class MegaPineFoliagePlacer : public FoliagePlacer {
private:
    std::shared_ptr<carver::IntProvider> m_crownHeight;

public:
    MegaPineFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        std::shared_ptr<carver::IntProvider> crownHeight
    )
        : FoliagePlacer(radius, offset)
        , m_crownHeight(crownHeight)
    {}

    /**
     * Reference: MegaPineFoliagePlacer.java foliageHeight() lines 45-47
     */
    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return m_crownHeight->sample(random);
    }

protected:
    /**
     * Reference: MegaPineFoliagePlacer.java createFoliage() lines 25-43
     */
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    /**
     * Reference: MegaPineFoliagePlacer.java shouldSkipLocation() lines 49-55
     */
    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        if (dx + dz >= 7) {
            return true;
        }
        return dx * dx + dz * dz > currentRadius * currentRadius;
    }
};

/**
 * RandomSpreadFoliagePlacer - Randomly scattered leaves (azalea)
 * Reference: RandomSpreadFoliagePlacer.java
 */
class RandomSpreadFoliagePlacer : public FoliagePlacer {
private:
    std::shared_ptr<carver::IntProvider> m_foliageHeight;
    int m_leafPlacementAttempts;

public:
    RandomSpreadFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        std::shared_ptr<carver::IntProvider> foliageHeight,
        int leafPlacementAttempts
    )
        : FoliagePlacer(radius, offset)
        , m_foliageHeight(foliageHeight)
        , m_leafPlacementAttempts(leafPlacementAttempts)
    {}

    /**
     * Reference: RandomSpreadFoliagePlacer.java foliageHeight() lines 38-40
     */
    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return m_foliageHeight->sample(random);
    }

protected:
    /**
     * Reference: RandomSpreadFoliagePlacer.java createFoliage() lines 27-36
     */
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    /**
     * Reference: RandomSpreadFoliagePlacer.java shouldSkipLocation() lines 42-44
     */
    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        return false;
    }
};

/**
 * CherryFoliagePlacer - Cherry tree foliage with hanging leaves
 * Reference: CherryFoliagePlacer.java
 */
class CherryFoliagePlacer : public FoliagePlacer {
private:
    std::shared_ptr<carver::IntProvider> m_height;
    float m_wideBottomLayerHoleChance;
    float m_cornerHoleChance;
    float m_hangingLeavesChance;
    float m_hangingLeavesExtensionChance;

public:
    CherryFoliagePlacer(
        std::shared_ptr<carver::IntProvider> radius,
        std::shared_ptr<carver::IntProvider> offset,
        std::shared_ptr<carver::IntProvider> height,
        float wideBottomLayerHoleChance,
        float cornerHoleChance,
        float hangingLeavesChance,
        float hangingLeavesExtensionChance
    )
        : FoliagePlacer(radius, offset)
        , m_height(height)
        , m_wideBottomLayerHoleChance(wideBottomLayerHoleChance)
        , m_cornerHoleChance(cornerHoleChance)
        , m_hangingLeavesChance(hangingLeavesChance)
        , m_hangingLeavesExtensionChance(hangingLeavesExtensionChance)
    {}

    /**
     * Reference: CherryFoliagePlacer.java foliageHeight() lines 48-50
     */
    int foliageHeight(WorldgenRandom& random, int treeHeight) const override {
        return m_height->sample(random);
    }

protected:
    /**
     * Reference: CherryFoliagePlacer.java createFoliage() lines 33-46
     */
    void createFoliageImpl(
        FoliageSetter& foliageSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    /**
     * Reference: CherryFoliagePlacer.java shouldSkipLocation() lines 52-64
     */
    bool shouldSkipLocation(
        WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override;
};

} // namespace foliageplacers
} // namespace feature
} // namespace levelgen
} // namespace minecraft
