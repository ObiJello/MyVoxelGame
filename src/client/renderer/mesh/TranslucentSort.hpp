// File: src/client/renderer/mesh/TranslucentSort.hpp
//
// Back-to-front ordering of translucent quads. Port of, in order:
//   com/mojang/blaze3d/vertex/MeshData          (unpackQuadCentroids, SortState)
//   com/mojang/blaze3d/vertex/VertexSorting     (byDistance)
//   net/minecraft/client/renderer/chunk/TranslucencyPointOfView
//   net/minecraft/client/renderer/LevelRenderer (scheduleResort throttling)
//
// WHY a translucent surface has to be drawn far-to-near: the translucent
// pass writes depth (MC TRANSLUCENT_TERRAIN keeps the default depth write),
// so every texel that survives the pass's cutout occludes whatever is drawn
// after it. Blending is only right if the far quads are already in the
// colour buffer when a near one lands — stained glass drawn before the
// water behind it hides that water outright.
//
// The cutout is what keeps CLEAR texels out of this: the translucent
// shader discards below MC 26.3's TRANSLUCENT_TERRAIN threshold of 0.1
// (26.1 had 0.01, which the flat +0.025 scaleAlphaToCoverage adds to
// cutout-strategy mips above level 0 could clear — MipmapGenerator.java).
// Glass's interior (alpha 0; glass.png.mcmeta mipmaps it by "mean") is
// therefore discarded at every mip and writes no depth. Without that cutout
// (the translucent shader once had none) a pane's invisible interior hid the
// water behind it whenever the pane's quad sorted first.
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

    // cameraPos is the WORLD camera (double, like MC's Vec3); sectionOrigin
    // is the section's minimum block corner.
    PointOfView MakePointOfView(const glm::dvec3& cameraPos, const glm::ivec3& sectionOrigin);

    // MC SectionRenderDispatcher.createVertexSorting: the camera relative to
    // the section's minimum corner, subtracted in double and only then
    // narrowed, so the sort keys keep full float precision anywhere in the
    // world. Centroids are section-relative too (the TerrainVertex positions
    // decoded without the origin), exactly as MC's are.
    inline glm::vec3 SectionRelativeCamera(const glm::dvec3& cameraPos, const glm::ivec3& sectionOrigin) {
        return glm::vec3(cameraPos - glm::dvec3(sectionOrigin));
    }

    // MC MeshData.unpackQuadCentroids: the midpoint of vertices 0 and 2, the
    // quad's diagonal. Assumes quad k owns vertices 4k..4k+3, which is what
    // Mesher::GenerateQuad and FluidMeshBuilder both emit.
    void ComputeCentroids(const std::vector<glm::vec3>& quadV0AndV2,
                          std::vector<glm::vec3>& outCentroids);

    // Rebuilds `outIndices` with the quads ordered farthest-first.
    // `centroids` and `cameraPos` share one frame: section-relative
    // (SectionRelativeCamera). Ties keep emission order (stable, as MC's
    // IntArrays.mergeSort), so equal keys never swap between sorts.
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
                            std::vector<uint32_t>& scratchOrder,
                            std::vector<float>& scratchKeys);

} // namespace Render::TranslucentSort
