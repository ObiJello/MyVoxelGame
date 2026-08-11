// File: src/client/renderer/mesh/TranslucentSort.hpp
//
// Back-to-front ordering of translucent quads. Port of, in order:
//   com/mojang/blaze3d/vertex/MeshData          (unpackQuadCentroids, SortState)
//   com/mojang/blaze3d/vertex/VertexSorting     (byDistance)
//   net/minecraft/client/renderer/chunk/TranslucencyPointOfView
//   net/minecraft/client/renderer/LevelRenderer (scheduleResort throttling)
//
// WHY a translucent surface has to be drawn far-to-near even when it looks
// opaque: the mipmap pipeline's scaleAlphaToCoverage adds a flat +0.025 to
// every texel's alpha above level 0 (MipmapGenerator.java, vanilla's own
// constant). Glass's interior is alpha 0 at level 0 but ~0.025 from level 1
// on, and the translucent pass's cutout threshold is 0.01 — so past the first
// mip the interior stops being discarded, draws at ~2.5% alpha (invisible) and
// WRITES DEPTH. Anything behind it is then occluded by a pane you cannot see.
//
// Vanilla has the identical texture data and the identical 0.01 threshold. It
// gets away with it purely because the far quads are already in the colour
// buffer by the time that invisible near one writes depth. Sorting is not a
// polish pass here; it is what makes depth-writing translucency work at all.
#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace Render::TranslucentSort {

    // MC TranslucencyPointOfView. The camera's section coordinate minus the
    // section's own, clamped per axis to [-1, 1] — i.e. which of the 27 cells
    // around the section the viewer is in. A section only needs re-sorting when
    // this changes, which is what keeps the cost off the frame budget.
    struct PointOfView {
        int8_t x = 0, y = 0, z = 0;
        bool valid = false;     // false until the section has been sorted once

        bool operator==(const PointOfView& o) const {
            return valid == o.valid && x == o.x && y == o.y && z == o.z;
        }
        bool operator!=(const PointOfView& o) const { return !(*this == o); }

        // MC: `this.x == 0 || this.y == 0 || this.z == 0`. Axis-aligned views
        // are the ones where a single block of camera movement can reorder
        // quads, so they re-sort more eagerly.
        bool IsAxisAligned() const { return x == 0 || y == 0 || z == 0; }
    };

    // sectionOrigin is the section's minimum block corner.
    PointOfView MakePointOfView(const glm::vec3& cameraPos, const glm::ivec3& sectionOrigin);

    // MC MeshData.unpackQuadCentroids: the midpoint of vertices 0 and 2, the
    // quad's diagonal. Assumes quad k owns vertices 4k..4k+3, which is what
    // Mesher::GenerateQuad and FluidMeshBuilder both emit.
    void ComputeCentroids(const std::vector<glm::vec3>& quadV0AndV2,
                          std::vector<glm::vec3>& outCentroids);

    // Rebuilds `outIndices` with the quads ordered farthest-first.
    //
    // MC sorts DESCENDING by squared distance (VertexSorting.byDistance ->
    // Floats.compare(keys[o2], keys[o1])) and re-emits each quad's six indices
    // from its start vertex. The winding pattern below is this engine's
    // (0,1,2),(0,2,3) rather than vanilla's (0,1,2),(2,3,0) — same two
    // triangles, but the engine's order has to be preserved or back-face
    // culling flips on every translucent quad.
    void BuildSortedIndices(const std::vector<glm::vec3>& centroids,
                            const glm::vec3& cameraPos,
                            std::vector<uint16_t>& outIndices,
                            std::vector<uint32_t>& scratchOrder);

} // namespace Render::TranslucentSort
