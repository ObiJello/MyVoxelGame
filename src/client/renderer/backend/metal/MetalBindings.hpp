// File: src/client/renderer/backend/metal/MetalBindings.hpp
#pragma once

#include <cstdint>

// The Metal argument indices the generated shaders use (shaders/metal/*.metal,
// tools/gen_metal_shaders.py). The generator rewrites every Vulkan
// (descriptor set, binding) to one of these before spirv-cross runs; the two
// tables must agree. Buffers and textures are separate index spaces in Metal,
// and each texture slot's sampler shares its texture's index.
namespace Render::MetalBindings {

    // Buffers (vertex and fragment stage alike)
    constexpr uint32_t kPushConstants = 0;   // PushConstantBlock, setVertexBytes / setFragmentBytes
    constexpr uint32_t kCommon        = 1;   // CommonUBO           (Vulkan set 1 binding 0)
    constexpr uint32_t kBones         = 2;   // BonesUBO            (set 1 binding 1)
    constexpr uint32_t kUserUniform   = 3;   // BindUniformBuffer   (set 3)
    constexpr uint32_t kOitParams     = 4;   // OitProjParams       (set 6 binding 3)
    constexpr uint32_t kPackUniforms  = 5;   // a shader pack program's uniform block (PackShaderDesc; set 1 binding 0)

    // Textures (+ samplers): BindTexture slots 0..3 at their own index —
    // slot 1 is set 2, slot 2 the texel buffer of set 4, slot 3 the lightmap
    // of set 5; slots 4 and 5 (the block atlas's sprite arrays, set 7
    // bindings 0 and 1) at indices 7 and 8, past Improved Transparency's
    // three.
    constexpr uint32_t kTextureSlots  = 6;
    constexpr uint32_t kSpriteArray   = 7;   // BindTexture slot 4 (set 7 binding 0)
    constexpr uint32_t kSpriteArray2  = 8;   // BindTexture slot 5 (set 7 binding 1)
    // A shader pack program binds its samplers at texture index = slot, 0..15
    // (its own pipelines; nothing of the engine's shares a draw with it).
    constexpr uint32_t kPackTextureSlots = 16;
    constexpr uint32_t kTextureIndices = 16; // the argument tables' size
    // Improved Transparency's set 6 samplers: depth bounds, coefficients 0, 1.
    constexpr uint32_t kOitTexture0   = 4;
    constexpr uint32_t kOitTextures   = 3;
    constexpr uint32_t TextureIndexOfSlot(uint32_t slot) {
        return slot == 4 ? kSpriteArray : slot == 5 ? kSpriteArray2 : slot;
    }

    // Vertex streams, at the top of the buffer table, clear of the uniforms.
    constexpr uint32_t kVertexStream   = 30;   // binding 0, per vertex
    constexpr uint32_t kInstanceStream = 29;   // binding 1, per instance

} // namespace Render::MetalBindings
