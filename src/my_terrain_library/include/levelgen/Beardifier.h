#pragma once

#include "levelgen/DensityFunction.h"
#include "math/Mth.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace minecraft {
namespace levelgen {

/**
 * TerrainAdjustment - How structures affect surrounding terrain
 * Reference: net/minecraft/world/level/levelgen/structure/TerrainAdjustment.java
 */
enum class TerrainAdjustment {
    NONE,           // No terrain adjustment
    BURY,           // Bury structure underground
    BEARD_THIN,     // Thin beard effect
    BEARD_BOX,      // Box-shaped beard
    ENCAPSULATE     // Encapsulate structure
};

/**
 * BoundingBox - Axis-aligned bounding box for structures
 * Reference: net/minecraft/world/level/levelgen/structure/BoundingBox.java
 */
struct BoundingBox {
    int minX, minY, minZ;
    int maxX, maxY, maxZ;

    BoundingBox() : minX(0), minY(0), minZ(0), maxX(0), maxY(0), maxZ(0) {}
    BoundingBox(int x1, int y1, int z1, int x2, int y2, int z2)
        : minX(x1), minY(y1), minZ(z1), maxX(x2), maxY(y2), maxZ(z2) {}

    bool isInside(int x, int y, int z) const {
        return x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ && z <= maxZ;
    }

    BoundingBox inflatedBy(int amount) const {
        return BoundingBox(minX - amount, minY - amount, minZ - amount,
                          maxX + amount, maxY + amount, maxZ + amount);
    }
};

/**
 * Rigid - A rigid structure piece that affects terrain
 * Reference: Beardifier.java lines 224-226
 */
struct Rigid {
    BoundingBox box;
    TerrainAdjustment terrainAdjustment;
    int groundLevelDelta;

    Rigid() : terrainAdjustment(TerrainAdjustment::NONE), groundLevelDelta(0) {}
    Rigid(const BoundingBox& b, TerrainAdjustment adj, int delta)
        : box(b), terrainAdjustment(adj), groundLevelDelta(delta) {}
};

/**
 * JigsawJunction - Junction point for jigsaw structures
 * Reference: net/minecraft/world/level/levelgen/structure/pools/JigsawJunction.java
 */
struct JigsawJunction {
    int sourceX;
    int sourceGroundY;
    int sourceZ;
    int deltaY;
    // StructureTemplatePool.Projection destProjection; // Not needed for terrain

    JigsawJunction() : sourceX(0), sourceGroundY(0), sourceZ(0), deltaY(0) {}
    JigsawJunction(int x, int groundY, int z, int dy)
        : sourceX(x), sourceGroundY(groundY), sourceZ(z), deltaY(dy) {}

    int getSourceX() const { return sourceX; }
    int getSourceGroundY() const { return sourceGroundY; }
    int getSourceZ() const { return sourceZ; }
};

/**
 * Beardifier - Adds density around structures to prevent terrain from cutting through them
 *
 * Reference: net/minecraft/world/level/levelgen/Beardifier.java
 *
 * This class:
 * - Adds "bearding" density around structures (pillager outposts, villages, etc.)
 * - Prevents caves from generating through important structures
 * - Uses structure bounding boxes to determine density modifications
 * - Precomputes a 24x24x24 BEARD_KERNEL for efficiency
 */
class Beardifier : public density::DensityFunction {
public:
    // Constants from Java (lines 23-24)
    static constexpr int BEARD_KERNEL_RADIUS = 12;
    static constexpr int BEARD_KERNEL_SIZE = 24;

private:
    std::vector<Rigid> m_pieces;
    std::vector<JigsawJunction> m_junctions;
    BoundingBox* m_affectedBox;  // nullptr if no structures affect this area

    // Static kernel (24 * 24 * 24 = 13824 floats)
    static float s_beardKernel[13824];
    static bool s_kernelInitialized;

    // Initialize the beard kernel (Reference: Beardifier.java lines 25-34)
    static void initializeKernel();

    // Kernel index calculation
    static int kernelIndex(int zi, int xi, int yi) {
        return zi * BEARD_KERNEL_SIZE * BEARD_KERNEL_SIZE + xi * BEARD_KERNEL_SIZE + yi;
    }

    // Check if coordinate is in kernel range (Reference: lines 210-212)
    static bool isInKernelRange(int xi) {
        return xi >= 0 && xi < BEARD_KERNEL_SIZE;
    }

    // Compute beard contribution for kernel initialization (Reference: lines 214-221)
    static double computeBeardContribution(int dx, int dy, int dz);
    static double computeBeardContribution(int dx, double dy, int dz);

    // Get beard contribution from kernel (Reference: lines 196-208)
    static double getBeardContribution(int dx, int dy, int dz, int yToGround);

    // Get bury contribution (Reference: lines 191-194)
    static double getBuryContribution(double dx, double dy, double dz);

public:
    /**
     * Empty beardifier singleton
     * Reference: Beardifier.java line 35
     */
    static Beardifier* EMPTY();

    /**
     * Constructor for beardifier with structure data
     * Reference: Beardifier.java lines 94-99
     */
    Beardifier(const std::vector<Rigid>& pieces,
               const std::vector<JigsawJunction>& junctions,
               const BoundingBox* affectedBox);

    /**
     * Default constructor - creates empty beardifier
     */
    Beardifier();

    virtual ~Beardifier();

    // Fast type checking for wrapNew()
    WrapType getWrapType() const override { return WrapType::Beardifier; }

    /**
     * Compute density modification at a position
     * Reference: Beardifier.java lines 110-181
     */
    double compute(const FunctionContext& context) const override;

    /**
     * Fill array with density values
     * Reference: Beardifier.java lines 101-108
     */
    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override;

    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }

    double minValue() const override {
        return -std::numeric_limits<double>::infinity();
    }

    double maxValue() const override {
        return std::numeric_limits<double>::infinity();
    }

    bool isEmpty() const {
        return m_affectedBox == nullptr;
    }
};

} // namespace levelgen
} // namespace minecraft
