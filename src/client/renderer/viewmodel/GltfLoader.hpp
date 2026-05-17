// File: src/client/renderer/viewmodel/GltfLoader.hpp
//
// Minimal glTF 2.0 (.glb binary form) loader. Built for the portal-gun
// viewmodel; reads:
//
//   • Per-primitive POSITION / NORMAL / TEXCOORD_0 / JOINTS_0 / WEIGHTS_0
//     (skinned-vertex layout) and SCALAR uint8/16/32 indices.
//   • Node hierarchy with TRS — needed to evaluate animations into
//     local→world bone transforms.
//   • Skins (joint node indices + inverse bind matrices).
//   • Animations — sampler keyframes + channels targeting node TRS.
//
// What this loader still does NOT do:
//   • Embedded image / sampler decoding (textures loaded by name
//     externally from pre-converted VTF→PNG files).
//   • CUBICSPLINE interpolation (LINEAR + STEP only — Portal's MDL
//     animations decompose to LINEAR keys at 30 fps after SourceIO
//     bake).
//   • .gltf (separate JSON + bin) — we only ship .glb.
//
// Entirely gated on ENABLE_PORTAL_GUN.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Render::Gltf {

    // Skinned vertex — must match PortalGunViewmodel's vertex layout.
    // 52 bytes: pos(12) + uv(8) + normal(12) + joints(4 as ubyte×4) + weights(16).
    // 4 × ubyte is sufficient — glb's JOINTS_0 is UBYTE and Portal's
    // largest skin has 45 joints, well under 255.
    struct Vertex {
        float    px, py, pz;
        float    u,  v;
        float    nx, ny, nz;
        uint8_t  joints[4];    // UBYTE per glTF spec
        float    weights[4];
    };
    static_assert(sizeof(Vertex) == 52, "Vertex must be 52 bytes (matches skinned VBO layout)");

    struct Primitive {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;
        std::string           materialName;
        int                   nodeIndex = -1;  // owning node (carries skin reference)
    };

    // Local-space transform of a single node in the glTF hierarchy.
    // Animations mutate translation / rotation / scale in place; the
    // renderer recomputes worldMatrix each frame from these.
    struct Node {
        std::string             name;
        glm::vec3               translation{0.0f};
        glm::quat               rotation{1.0f, 0.0f, 0.0f, 0.0f};   // (w, x, y, z)
        glm::vec3               scale{1.0f};
        int                     parent     = -1;
        std::vector<int>        children;
        int                     skinIndex  = -1;
        int                     meshIndex  = -1;
        // Cached identity-fallback when a node has a matrix instead of
        // TRS — glTF spec allows either form, exporters use one or the
        // other. We decompose matrix→TRS at load time so the rest of
        // the renderer only ever sees TRS.
    };

    struct Skin {
        std::vector<int>       jointNodeIndices;   // glTF node indices that are joints
        std::vector<glm::mat4> inverseBindMatrices; // per joint, model→bone-space at bind pose
    };

    // One keyframe channel — output type is implied by the channel's
    // path (translation/scale = vec3, rotation = quat as vec4).
    struct AnimSampler {
        std::vector<float> input;        // keyframe times, monotonically increasing
        // values laid out as `componentsPerKey` floats per keyframe.
        std::vector<float> output;
        int                componentsPerKey = 3;
        // Only LINEAR + STEP supported; CUBICSPLINE downgrades to LINEAR.
        bool               isStep = false;
    };

    struct AnimChannel {
        int          targetNode = -1;
        // 0 = translation, 1 = rotation, 2 = scale.
        int          path       = 0;
        int          samplerIdx = -1;
    };

    struct Animation {
        std::string              name;
        float                    duration = 0.0f;  // seconds
        std::vector<AnimSampler> samplers;
        std::vector<AnimChannel> channels;
    };

    struct Model {
        std::vector<Primitive>  primitives;
        std::vector<Node>       nodes;
        std::vector<Skin>       skins;
        std::vector<Animation>  animations;
    };

    // Load a .glb from disk. Returns an empty Model on failure (caller
    // should skip rendering rather than crash — viewmodel is cosmetic).
    Model LoadGLB(const std::string& path);

} // namespace Render::Gltf

#endif // ENABLE_PORTAL_GUN
