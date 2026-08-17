// File: src/client/renderer/mesh/TranslucentSort.cpp
#include "TranslucentSort.hpp"

#include <algorithm>
#include <cmath>

namespace Render::TranslucentSort {

    namespace {
        // MC SectionPos.blockToSectionCoord — arithmetic shift so negatives
        // floor rather than truncate toward zero.
        inline int BlockToSection(float v) {
            return static_cast<int>(std::floor(v)) >> 4;
        }
    }

    PointOfView MakePointOfView(const glm::vec3& cameraPos, const glm::ivec3& sectionOrigin) {
        auto axis = [](float camera, int originBlock) -> int8_t {
            const int rel = BlockToSection(camera) - (originBlock >> 4);
            return static_cast<int8_t>(std::clamp(rel, -1, 1));
        };
        PointOfView pov;
        pov.x = axis(cameraPos.x, sectionOrigin.x);
        pov.y = axis(cameraPos.y, sectionOrigin.y);
        pov.z = axis(cameraPos.z, sectionOrigin.z);
        pov.valid = true;
        return pov;
    }

    void ComputeCentroids(const std::vector<glm::vec3>& quadV0AndV2,
                          std::vector<glm::vec3>& outCentroids) {
        // Input is packed as [v0, v2] per quad.
        const size_t quads = quadV0AndV2.size() / 2;
        outCentroids.resize(quads);
        for (size_t q = 0; q < quads; ++q) {
            outCentroids[q] = (quadV0AndV2[q * 2] + quadV0AndV2[q * 2 + 1]) * 0.5f;
        }
    }

    void BuildSortedIndices(const std::vector<glm::vec3>& centroids,
                            const glm::vec3& cameraPos,
                            std::vector<uint16_t>& outIndices,
                            std::vector<uint32_t>& scratchOrder,
                            std::vector<float>& scratchKeys) {
        const size_t quads = centroids.size();
        outIndices.clear();
        if (quads == 0) return;

        // Precompute a flat key array in ONE sequential pass, then sort indices
        // by comparing those floats. This is exactly what MC does — see
        // VertexSorting.byDistance (com/mojang/blaze3d/vertex/VertexSorting.java):
        //
        //     float[] keys = new float[values.size()];
        //     for (int i = 0; i < values.size(); indices[i] = i++)
        //         keys[i] = function.apply(values.get(i, scratch));
        //     IntArrays.mergeSort(indices, (a, b) -> Floats.compare(keys[b], keys[a]));
        //
        // We used to recompute the vector subtract and dot INSIDE the comparator,
        // which costs ~2*N*log(N) distance evaluations instead of N — about 44k
        // instead of 2k for a 2000-quad section — and every one of them was a
        // 12-byte random-access load into `centroids` rather than a 4-byte
        // sequential one. That comparator was the bulk of ResortTranslucent.
        //
        // Squared distance is enough: it is monotonic in distance, and MC uses
        // distanceSquared for exactly this reason.
        scratchKeys.resize(quads);
        scratchOrder.resize(quads);
        for (size_t i = 0; i < quads; ++i) {
            const glm::vec3 d = centroids[i] - cameraPos;
            scratchKeys[i] = glm::dot(d, d);
            scratchOrder[i] = static_cast<uint32_t>(i);
        }

        std::stable_sort(scratchOrder.begin(), scratchOrder.end(),
                         [&scratchKeys](uint32_t a, uint32_t b) {
                             return scratchKeys[a] > scratchKeys[b];  // farthest first
                         });

        // Every quad is re-emitted with the SAME forward winding, so every quad
        // in a translucent buffer must already use it — a quad that encodes its
        // facing by reversing its indices instead of its vertices gets re-wound
        // here and then back-face culled out of existence. See the backward-up
        // face in FluidMeshBuilder for the one place that has to care.
        outIndices.reserve(quads * 6);
        for (uint32_t q : scratchOrder) {
            const uint32_t base = q * 4u;
            // Must stay inside the 16-bit index space the mega-buffer uses;
            // the mesher caps a layer at 65,536 vertices for the same reason.
            if (base + 3u > 0xFFFFu) continue;
            const uint16_t b0 = static_cast<uint16_t>(base);
            outIndices.insert(outIndices.end(), {
                static_cast<uint16_t>(b0 + 0), static_cast<uint16_t>(b0 + 1),
                static_cast<uint16_t>(b0 + 2),
                static_cast<uint16_t>(b0 + 0), static_cast<uint16_t>(b0 + 2),
                static_cast<uint16_t>(b0 + 3)
            });
        }
    }

} // namespace Render::TranslucentSort
