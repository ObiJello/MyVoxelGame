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
                            std::vector<uint32_t>& scratchOrder) {
        const size_t quads = centroids.size();
        outIndices.clear();
        if (quads == 0) return;

        scratchOrder.resize(quads);
        for (size_t i = 0; i < quads; ++i) scratchOrder[i] = static_cast<uint32_t>(i);

        // Squared distance is enough: it is monotonic in distance, and MC uses
        // distanceSquared for exactly this reason.
        std::stable_sort(scratchOrder.begin(), scratchOrder.end(),
                         [&](uint32_t a, uint32_t b) {
                             const glm::vec3 da = centroids[a] - cameraPos;
                             const glm::vec3 db = centroids[b] - cameraPos;
                             return glm::dot(da, da) > glm::dot(db, db);  // farthest first
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
