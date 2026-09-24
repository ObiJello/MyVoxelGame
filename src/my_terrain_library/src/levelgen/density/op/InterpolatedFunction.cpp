#include "levelgen/density/op/InterpolatedFunction.h"

#include "levelgen/density/JavaMath.h"
#include "levelgen/density/SamplerContext.h"

#include <algorithm>

// Reference: op.InterpolatedFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// InterpolatedFunction.Sampler.
class InterpolatedSampler final : public DensitySampler {
public:
    InterpolatedSampler(DensitySamplerPtr input, int cellSizeXz, int cellSizeY, float cellSizeXzInv,
                        float cellSizeYInv)
        : m_input(std::move(input)), m_cellSizeXz(cellSizeXz), m_cellSizeY(cellSizeY),
          m_cellSizeXzInv(cellSizeXzInv), m_cellSizeYInv(cellSizeYInv) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        if ((volume.stepBlockX == m_cellSizeXz || volume.sizeX == 1) &&
            (volume.stepBlockY == m_cellSizeY || volume.sizeY == 1) &&
            (volume.stepBlockZ == m_cellSizeXz || volume.sizeZ == 1) &&
            jmath::floorMod(volume.minBlockX, m_cellSizeXz) == 0 &&
            jmath::floorMod(volume.minBlockY, m_cellSizeY) == 0 &&
            jmath::floorMod(volume.minBlockZ, m_cellSizeXz) == 0) {
            m_input->sampleVolume(context, outputBuffer, volume);
        } else if (volume.stepBlockX == 1 && volume.stepBlockY == 1 && volume.stepBlockZ == 1) {
            sampleWithBlockStep(context, outputBuffer, volume);
        } else {
            sampleWithNonBlockStep(context, outputBuffer, volume);
        }
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const int xInCell = jmath::floorMod(blockX, m_cellSizeXz);
        const int yInCell = jmath::floorMod(blockY, m_cellSizeY);
        const int zInCell = jmath::floorMod(blockZ, m_cellSizeXz);
        if (xInCell == 0 && yInCell == 0 && zInCell == 0) {
            return m_input->sampleValue(context, blockX, blockY, blockZ);
        }
        const DensityVolume volume(2, 2, 2, blockX - xInCell, blockY - yInCell, blockZ - zInCell,
                                   m_cellSizeXz, m_cellSizeY, m_cellSizeXz);
        ScopedBuffer inputBuffer = context.acquireBuffer(volume);
        m_input->sampleVolume(context, *inputBuffer, volume);
        return jmath::lerp3(static_cast<float>(xInCell) / static_cast<float>(m_cellSizeXz),
                            static_cast<float>(yInCell) / static_cast<float>(m_cellSizeY),
                            static_cast<float>(zInCell) / static_cast<float>(m_cellSizeXz),
                            inputBuffer->get(volume.indexUnchecked(0, 0, 0)),
                            inputBuffer->get(volume.indexUnchecked(1, 0, 0)),
                            inputBuffer->get(volume.indexUnchecked(0, 1, 0)),
                            inputBuffer->get(volume.indexUnchecked(1, 1, 0)),
                            inputBuffer->get(volume.indexUnchecked(0, 0, 1)),
                            inputBuffer->get(volume.indexUnchecked(1, 0, 1)),
                            inputBuffer->get(volume.indexUnchecked(0, 1, 1)),
                            inputBuffer->get(volume.indexUnchecked(1, 1, 1)));
    }

private:
    void sampleWithNonBlockStep(SamplerContext& context, DensityBuffer& outputBuffer,
                                const DensityVolume& volume) const {
        const DensityVolume blockVolume(volume.sizeX * volume.stepBlockX, volume.sizeY * volume.stepBlockY,
                                        volume.sizeZ * volume.stepBlockZ, volume.minBlockX, volume.minBlockY,
                                        volume.minBlockZ, 1, 1, 1);
        ScopedBuffer blockBuffer = context.acquireBuffer(blockVolume);
        sampleWithBlockStep(context, *blockBuffer, blockVolume);
        for (int z = 0; z < volume.sizeZ; ++z) {
            for (int x = 0; x < volume.sizeX; ++x) {
                for (int y = 0; y < volume.sizeY; ++y) {
                    const float value = blockBuffer->get(blockVolume.indexUnchecked(
                        x * volume.stepBlockX, y * volume.stepBlockY, z * volume.stepBlockZ));
                    outputBuffer.set(volume.indexUnchecked(x, y, z), value);
                }
            }
        }
    }

    void sampleWithBlockStep(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const {
        const int minCellX = jmath::floorDiv(volume.minBlockX, m_cellSizeXz);
        const int minCellY = jmath::floorDiv(volume.minBlockY, m_cellSizeY);
        const int minCellZ = jmath::floorDiv(volume.minBlockZ, m_cellSizeXz);
        const int maxCellX = jmath::floorDiv(volume.maxBlockX(), m_cellSizeXz);
        const int maxCellY = jmath::floorDiv(volume.maxBlockY(), m_cellSizeY);
        const int maxCellZ = jmath::floorDiv(volume.maxBlockZ(), m_cellSizeXz);
        const int cellCountX = maxCellX - minCellX + 1;
        const int cellCountY = maxCellY - minCellY + 1;
        const int cellCountZ = maxCellZ - minCellZ + 1;
        const DensityVolume cellVolume(
            jmath::floorMod(volume.maxBlockX(), m_cellSizeXz) == 0 ? cellCountX : cellCountX + 1,
            jmath::floorMod(volume.maxBlockY(), m_cellSizeY) == 0 ? cellCountY : cellCountY + 1,
            jmath::floorMod(volume.maxBlockZ(), m_cellSizeXz) == 0 ? cellCountZ : cellCountZ + 1,
            minCellX * m_cellSizeXz, minCellY * m_cellSizeY, minCellZ * m_cellSizeXz,
            m_cellSizeXz, m_cellSizeY, m_cellSizeXz);
        ScopedBuffer cellBuffer = context.acquireBuffer(cellVolume);
        m_input->sampleVolume(context, *cellBuffer, cellVolume);
        for (int cellZ = 0; cellZ < cellCountZ; ++cellZ) {
            const int nextCellZ = std::min(cellZ + 1, cellVolume.sizeZ - 1);
            for (int cellX = 0; cellX < cellCountX; ++cellX) {
                const int nextCellX = std::min(cellX + 1, cellVolume.sizeX - 1);
                float v000 = cellBuffer->get(cellVolume.indexUnchecked(cellX, 0, cellZ));
                float v100 = cellBuffer->get(cellVolume.indexUnchecked(nextCellX, 0, cellZ));
                float v001 = cellBuffer->get(cellVolume.indexUnchecked(cellX, 0, nextCellZ));
                float v101 = cellBuffer->get(cellVolume.indexUnchecked(nextCellX, 0, nextCellZ));
                for (int cellY = 0; cellY < cellCountY; ++cellY) {
                    const int nextCellY = std::min(cellY + 1, cellVolume.sizeY - 1);
                    const float v010 = cellBuffer->get(cellVolume.indexUnchecked(cellX, nextCellY, cellZ));
                    const float v110 = cellBuffer->get(cellVolume.indexUnchecked(nextCellX, nextCellY, cellZ));
                    const float v011 = cellBuffer->get(cellVolume.indexUnchecked(cellX, nextCellY, nextCellZ));
                    const float v111 = cellBuffer->get(cellVolume.indexUnchecked(nextCellX, nextCellY, nextCellZ));
                    fillCell(outputBuffer, volume, cellVolume, cellX, cellY, cellZ,
                             v000, v100, v010, v110, v001, v101, v011, v111);
                    v000 = v010;
                    v100 = v110;
                    v001 = v011;
                    v101 = v111;
                }
            }
        }
    }

    void fillCell(DensityBuffer& outputBuffer, const DensityVolume& outputVolume, const DensityVolume& cellVolume,
                  int cellX, int cellY, int cellZ, float v000, float v100, float v010, float v110,
                  float v001, float v101, float v011, float v111) const {
        const int cellOutputX = cellVolume.blockX(cellX) - outputVolume.minBlockX;
        const int cellOutputY = cellVolume.blockY(cellY) - outputVolume.minBlockY;
        const int cellOutputZ = cellVolume.blockZ(cellZ) - outputVolume.minBlockZ;
        const int x0 = std::max(0, -cellOutputX);
        const int y0 = std::max(0, -cellOutputY);
        const int z0 = std::max(0, -cellOutputZ);
        const int x1 = std::min(m_cellSizeXz, outputVolume.sizeX - cellOutputX) - 1;
        const int y1 = std::min(m_cellSizeY, outputVolume.sizeY - cellOutputY) - 1;
        const int z1 = std::min(m_cellSizeXz, outputVolume.sizeZ - cellOutputZ) - 1;
        for (int z = z0; z <= z1; ++z) {
            const int outputZ = cellOutputZ + z;
            const float alphaZ = static_cast<float>(z) * m_cellSizeXzInv;
            const float v00_ = jmath::lerp(alphaZ, v000, v001);
            const float v01_ = jmath::lerp(alphaZ, v010, v011);
            const float v10_ = jmath::lerp(alphaZ, v100, v101);
            const float v11_ = jmath::lerp(alphaZ, v110, v111);
            for (int x = x0; x <= x1; ++x) {
                const int outputX = cellOutputX + x;
                const float alphaX = static_cast<float>(x) * m_cellSizeXzInv;
                const float v_0_ = jmath::lerp(alphaX, v00_, v10_);
                const float v_1_ = jmath::lerp(alphaX, v01_, v11_);
                const float valueStep = (v_1_ - v_0_) * m_cellSizeYInv;
                // Accumulated, not recomputed per y: Java's rounding.
                float value = v_0_ + valueStep * static_cast<float>(y0);
                int outputIndex = outputVolume.indexUnchecked(outputX, cellOutputY + y0, outputZ);
                for (int y = y0; y <= y1; ++y) {
                    outputBuffer.set(outputIndex++, value);
                    value += valueStep;
                }
            }
        }
    }

    DensitySamplerPtr m_input;
    int m_cellSizeXz;
    int m_cellSizeY;
    float m_cellSizeXzInv;
    float m_cellSizeYInv;
};

} // namespace

DensitySamplerPtr InterpolatedFunction::compileSampler(CompileContext& context) const {
    return std::make_shared<InterpolatedSampler>(m_input->compileSampler(context), m_cellSizeXz, m_cellSizeY,
                                                 1.0f / static_cast<float>(m_cellSizeXz),
                                                 1.0f / static_cast<float>(m_cellSizeY));
}

DensityFunctionPtr InterpolatedFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    return input == m_input ? self() : std::make_shared<InterpolatedFunction>(input, m_cellSizeXz, m_cellSizeY);
}

bool InterpolatedFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const InterpolatedFunction*>(&other);
    return o != nullptr && functionsEqual(m_input, o->m_input) && m_cellSizeXz == o->m_cellSizeXz &&
           m_cellSizeY == o->m_cellSizeY;
}

size_t InterpolatedFunction::hash() const {
    size_t h = hashCombine(0x34, m_input->hash());
    h = hashCombine(h, std::hash<int>()(m_cellSizeXz));
    return hashCombine(h, std::hash<int>()(m_cellSizeY));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
