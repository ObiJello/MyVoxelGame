#pragma once

#include "levelgen/Beardifier.h"
#include "levelgen/density/DensitySampler.h"

#include <optional>
#include <vector>

// Reference: levelgen.Beardifier (26.3) - the terrain adaptation around
// structures (rigid pieces and jigsaw junctions), now a float DensitySampler
// read through the "beardifier" context key. The piece and junction lists are
// collected exactly as before (Beardifier.forStructuresInChunk); the rigid,
// junction and bounding-box types are the engine's levelgen:: ones.

namespace minecraft {
namespace levelgen {
namespace density {

class Beardifier : public DensitySampler {
public:
    Beardifier(std::vector<levelgen::Rigid> pieces, std::vector<levelgen::JigsawJunction> junctions,
               std::optional<levelgen::BoundingBox> affectedBox)
        : m_pieces(std::move(pieces)), m_junctions(std::move(junctions)), m_affectedBox(affectedBox) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                      const DensityVolume& volume) const override;
    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override;

    bool isEmpty() const { return !m_affectedBox.has_value(); }

protected:
    float sampleValueUnchecked(int blockX, int blockY, int blockZ) const;

private:
    static float getBuryContribution(float dx, float dy, float dz);
    static float getBeardContribution(int dx, int dy, int dz, int yToGround);

    std::vector<levelgen::Rigid> m_pieces;
    std::vector<levelgen::JigsawJunction> m_junctions;
    std::optional<levelgen::BoundingBox> m_affectedBox;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
