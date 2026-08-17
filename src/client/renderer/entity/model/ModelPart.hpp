// File: src/client/renderer/entity/model/ModelPart.hpp
//
// MC net.minecraft.client.model.geom.{ModelPart, PartPose, builders.*}.
//
// ── Why this exists ────────────────────────────────────────────────────────
//
// Three independent copies of MC's cube-with-texOffs algorithm already live in
// this codebase: ChestRenderer::AddCube, ShulkerBoxRenderer::AddCube (a verbatim
// duplicate), and HeadItemRenderer::BuildCubeFaces. ShulkerBoxRenderer.cpp:58-63
// says outright that a third entity-model renderer is the moment to lift them.
// Mob rendering is that third user, so the geometry lives here now.
//
// Two things this generalises beyond those copies:
//
//   * TEXTURE SIZE. Both block-entity copies hard-code a 64x64 atlas by
//     dividing UVs by 64 in the shader. Mob textures are 64x64 (zombie, cow)
//     AND 64x32 (chicken, sheep wool), so the divisor has to be per-model.
//
//   * A PART HIERARCHY. A chest is two rigid boxes; a mob is a tree — the head
//     rotates about the neck, and MC composes each part's transform from its
//     PartPose and its parent's. Flattening that would make every animation a
//     hand-written matrix.
//
// ── Coordinate space ───────────────────────────────────────────────────────
//
// Everything here is in MC MODEL SPACE: pixels (16 per block), Y DOWN, origin
// at the model root. The renderer applies MC's `scale(-1, -1, 1)` and the
// -1.501 Y offset to convert to world space, exactly as LivingEntityRenderer
// does. Authoring in MC's space is what lets createMesh() be copied verbatim
// from the decompiled client models with no per-number conversion — and a
// conversion applied per number is a conversion applied wrongly somewhere.
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Render {

    // Same layout as the block vertex, so parts stream into an ordinary vertex
    // buffer with GetBlockVertexLayout().
    struct ModelVertex {
        float   x, y, z;
        float   u, v;              // in TEXTURE PIXELS, divided in the shader
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(ModelVertex) == 24, "ModelVertex must match the block vertex stride");

    // MC PartPose — the rest transform of a part relative to its parent.
    struct PartPose {
        float x = 0.0f, y = 0.0f, z = 0.0f;             // pixels
        float xRot = 0.0f, yRot = 0.0f, zRot = 0.0f;    // radians

        static PartPose Offset(float x, float y, float z) { return { x, y, z, 0, 0, 0 }; }
        static PartPose OffsetAndRotation(float x, float y, float z,
                                          float xRot, float yRot, float zRot) {
            return { x, y, z, xRot, yRot, zRot };
        }
        static PartPose Zero() { return {}; }
    };

    // MC CubeDeformation — inflates a cube uniformly. Used for hat layers and
    // for the outer layers on drowned/stray, and by baby models.
    struct CubeDeformation {
        float grow = 0.0f;
        explicit CubeDeformation(float g = 0.0f) : grow(g) {}
    };

    // One cuboid within a part. Mirrors CubeListBuilder::addBox arguments.
    struct CubeDefinition {
        float originX, originY, originZ;    // pixels, MC model space
        float sizeX, sizeY, sizeZ;          // pixels
        float texOffsX, texOffsY;           // texture pixels
        float grow = 0.0f;
        bool  mirror = false;               // MC CubeListBuilder.mirror()
    };

    class ModelPart {
    public:
        ModelPart() = default;

        // MC ModelPart's mutable animation state. `setupAnim` writes these and
        // nothing else; the rest pose lives in `pose`.
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float xRot = 0.0f, yRot = 0.0f, zRot = 0.0f;
        float xScale = 1.0f, yScale = 1.0f, zScale = 1.0f;
        bool  visible = true;
        // MC ModelPart.skipDraw — skip THIS part's own cubes while still
        // drawing its children. A rolled-up armadillo hides its body but keeps
        // the head, which is parented to it.
        bool  skipDraw = false;

        PartPose pose;

        std::vector<CubeDefinition> cubes;
        std::vector<std::unique_ptr<ModelPart>> children;
        std::string name;

        // Reset the animation state to the rest pose. MC calls this via
        // loadPose at the start of every setupAnim.
        void ResetPose();

        ModelPart* AddChild(const std::string& childName, PartPose childPose);
        ModelPart* Find(const std::string& childName);

        // This part's local transform: rest pose composed with animation state.
        //
        // `extraTranslation` is added to x/y/z before the rotations, in pixels.
        // It exists for MC's translateToHand overrides, which shove the arm a
        // pixel sideways for the duration of one matrix build and put it back
        // (SkeletonModel.translateToHand) — passing it here is the same result
        // without mutating the part mid-frame.
        glm::mat4 LocalMatrix(const glm::vec3& extraTranslation = glm::vec3(0.0f)) const;

        // Append this part's cubes (and its children's) to `verts`/`idx`,
        // already transformed into model space by `parent`.
        void Build(const glm::mat4& parent, float texWidth, float texHeight,
                   std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx) const;
    };

    // Emit the six faces of one cuboid. Lifted from ChestRenderer::AddCube —
    // including the vanilla UV quirk where vertex 0 takes the HIGH u, which
    // mirrors every face if written the intuitive way round.
    void BuildCube(const CubeDefinition& cube, const glm::mat4& transform,
                   std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx);

} // namespace Render
