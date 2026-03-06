#include "levelgen/Beardifier.h"
#include <cstring>
#include <algorithm>

namespace minecraft {
namespace levelgen {

// Static member definitions
float Beardifier::s_beardKernel[13824];
bool Beardifier::s_kernelInitialized = false;

// Reference: Beardifier.java lines 25-34
void Beardifier::initializeKernel() {
    if (s_kernelInitialized) return;

    for (int zi = 0; zi < BEARD_KERNEL_SIZE; ++zi) {
        for (int xi = 0; xi < BEARD_KERNEL_SIZE; ++xi) {
            for (int yi = 0; yi < BEARD_KERNEL_SIZE; ++yi) {
                s_beardKernel[kernelIndex(zi, xi, yi)] =
                    static_cast<float>(computeBeardContribution(
                        xi - BEARD_KERNEL_RADIUS,
                        yi - BEARD_KERNEL_RADIUS,
                        zi - BEARD_KERNEL_RADIUS
                    ));
            }
        }
    }

    s_kernelInitialized = true;
}

// Reference: Beardifier.java lines 214-216
double Beardifier::computeBeardContribution(int dx, int dy, int dz) {
    return computeBeardContribution(dx, static_cast<double>(dy) + 0.5, dz);
}

// Reference: Beardifier.java lines 218-222
double Beardifier::computeBeardContribution(int dx, double dy, int dz) {
    double distanceSqr = Mth::lengthSquared(static_cast<double>(dx), dy, static_cast<double>(dz));
    double pieceWeight = std::exp(-distanceSqr / 16.0);
    return pieceWeight;
}

// Reference: Beardifier.java lines 196-208
double Beardifier::getBeardContribution(int dx, int dy, int dz, int yToGround) {
    int xi = dx + BEARD_KERNEL_RADIUS;
    int yi = dy + BEARD_KERNEL_RADIUS;
    int zi = dz + BEARD_KERNEL_RADIUS;

    if (isInKernelRange(xi) && isInKernelRange(yi) && isInKernelRange(zi)) {
        double dyWithOffset = static_cast<double>(yToGround) + 0.5;
        double distanceSqr = Mth::lengthSquared(
            static_cast<double>(dx), dyWithOffset, static_cast<double>(dz)
        );
        double value = -dyWithOffset * Mth::fastInvSqrt(distanceSqr / 2.0) / 2.0;
        return value * static_cast<double>(s_beardKernel[kernelIndex(zi, xi, yi)]);
    } else {
        return 0.0;
    }
}

// Reference: Beardifier.java lines 191-194
double Beardifier::getBuryContribution(double dx, double dy, double dz) {
    double distance = Mth::length(dx, dy, dz);
    return Mth::clampedMap(distance, 0.0, 6.0, 1.0, 0.0);
}

// Empty beardifier singleton
// Reference: Beardifier.java line 35
Beardifier* Beardifier::EMPTY() {
    static Beardifier empty;
    return &empty;
}

// Default constructor - creates empty beardifier
Beardifier::Beardifier()
    : m_affectedBox(nullptr)
{
    initializeKernel();
}

// Constructor with structure data
// Reference: Beardifier.java lines 94-99
Beardifier::Beardifier(
    const std::vector<Rigid>& pieces,
    const std::vector<JigsawJunction>& junctions,
    const BoundingBox* affectedBox
)
    : m_pieces(pieces)
    , m_junctions(junctions)
    , m_affectedBox(affectedBox ? new BoundingBox(*affectedBox) : nullptr)
{
    initializeKernel();
}

Beardifier::~Beardifier() {
    if (m_affectedBox) {
        delete m_affectedBox;
        m_affectedBox = nullptr;
    }
}

// Reference: Beardifier.java lines 101-108
void Beardifier::fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const {
    if (m_affectedBox == nullptr) {
        // Fill with zeros for empty beardifier
        std::memset(output, 0, count * sizeof(double));
    } else {
        // Use default fillArray behavior
        for (int32_t i = 0; i < count; ++i) {
            output[i] = compute(*contextProvider.forIndex(i));
        }
    }
}

// Reference: Beardifier.java lines 110-181
double Beardifier::compute(const FunctionContext& context) const {
    // Line 111-113: Early exit if no affected box
    if (m_affectedBox == nullptr) {
        return 0.0;
    }

    int blockX = context.blockX();
    int blockY = context.blockY();
    int blockZ = context.blockZ();

    // Line 117-118: Check if position is inside affected area
    if (!m_affectedBox->isInside(blockX, blockY, blockZ)) {
        return 0.0;
    }

    double noiseValue = 0.0;

    // Line 122-169: Process rigid pieces
    for (const Rigid& rigid : m_pieces) {
        const BoundingBox& box = rigid.box;
        int groundLevelDelta = rigid.groundLevelDelta;

        // Line 125-126: Calculate horizontal distance to box
        int dx = std::max(0, std::max(box.minX - blockX, blockX - box.maxX));
        int dz = std::max(0, std::max(box.minZ - blockZ, blockZ - box.maxZ));

        // Line 127-128: Calculate ground Y and vertical distance
        int groundY = box.minY + groundLevelDelta;
        int dyToGround = blockY - groundY;

        // Line 129-147: Calculate dy based on terrain adjustment type
        int dy;
        switch (rigid.terrainAdjustment) {
            case TerrainAdjustment::NONE:
                dy = 0;
                break;
            case TerrainAdjustment::BURY:
            case TerrainAdjustment::BEARD_THIN:
                dy = dyToGround;
                break;
            case TerrainAdjustment::BEARD_BOX:
                dy = std::max(0, std::max(groundY - blockY, blockY - box.maxY));
                break;
            case TerrainAdjustment::ENCAPSULATE:
                dy = std::max(0, std::max(box.minY - blockY, blockY - box.maxY));
                break;
            default:
                dy = 0;
                break;
        }

        // Line 149-166: Calculate contribution based on terrain adjustment type
        double contribution;
        switch (rigid.terrainAdjustment) {
            case TerrainAdjustment::NONE:
                contribution = 0.0;
                break;
            case TerrainAdjustment::BURY:
                contribution = getBuryContribution(
                    static_cast<double>(dx),
                    static_cast<double>(dy) / 2.0,
                    static_cast<double>(dz)
                );
                break;
            case TerrainAdjustment::BEARD_THIN:
            case TerrainAdjustment::BEARD_BOX:
                contribution = getBeardContribution(dx, dy, dz, dyToGround) * 0.8;
                break;
            case TerrainAdjustment::ENCAPSULATE:
                contribution = getBuryContribution(
                    static_cast<double>(dx) / 2.0,
                    static_cast<double>(dy) / 2.0,
                    static_cast<double>(dz) / 2.0
                ) * 0.8;
                break;
            default:
                contribution = 0.0;
                break;
        }

        noiseValue += contribution;
    }

    // Line 171-176: Process jigsaw junctions
    for (const JigsawJunction& junction : m_junctions) {
        int dx = blockX - junction.getSourceX();
        int dy = blockY - junction.getSourceGroundY();
        int dz = blockZ - junction.getSourceZ();
        noiseValue += getBeardContribution(dx, dy, dz, dy) * 0.4;
    }

    return noiseValue;
}

} // namespace levelgen
} // namespace minecraft
