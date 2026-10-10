// File: src/client/shader/PackCompiler.hpp
//
// The run-time shader compiler the Vulkan and Metal backends need for shader
// packs (docs/shader-packs-port.md): a pack program is GLSL, and neither API
// compiles GLSL. glslang turns the pipeline's Vulkan-GLSL translation
// (ShaderPackGlsl::Vulkanize) into SPIR-V, SPIRV-Cross reflects the program's
// uniform block — every uniform the pipeline sets by name, at its std140
// offset — and its samplers, and for Metal translates the SPIR-V to MSL.
//
// Built only with SHADER_PACK_SPIRV (CMake; HAS_PACK_SPIRV). Without it
// Available() is false and the pipeline reports that packs need OpenGL.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Shaders::PackCompiler {

    // A member of the program's uniform block, as the pipeline addresses it.
    struct Uniform {
        enum Kind { Float, Int, Bool, Vec2, Vec3, Vec4, IVec2, IVec3, IVec4, Mat3, Mat4, Other };
        std::string name;
        Kind        kind = Other;
        uint32_t    offset = 0;       // bytes into the block
        uint32_t    size = 0;         // bytes the member occupies (arrays included)
        uint32_t    arrayStride = 0;  // 0 when not an array
        uint32_t    arrayLength = 1;
    };
    struct Sampler {
        std::string name;
        uint32_t    binding = 0;      // the texture slot (descriptor set 0)
        bool        plain2D = true;   // sampler2D / sampler2DShadow (not an array, buffer or 3D)
        bool        vertexStage = false;
        bool        fragmentStage = false;
    };
    struct Program {
        std::vector<uint32_t> vertexSpirv;
        std::vector<uint32_t> fragmentSpirv;
        std::vector<Uniform>  uniforms;     // the union over both stages, by name
        uint32_t              uniformBlockSize = 0;
        std::vector<Sampler>  samplers;
        std::vector<uint32_t> vertexInputLocations;   // the vertex stage's attribute locations
        std::string error;
    };

    // Descriptor set 1, binding 0 is the uniform block; set 0 the samplers,
    // one binding per texture slot; set 3 binding 0 the engine's user
    // uniform block (ShaderPackGlsl::Vulkanize writes them all).
    constexpr uint32_t kUniformSet = 1;
    constexpr uint32_t kSamplerSet = 0;
    constexpr uint32_t kUserUniformSet = 3;
    constexpr uint32_t kMaxSamplers = 16;
    constexpr const char* kUniformBlockName = "PackUniforms";

    bool Available();

    // Vulkan GLSL in, SPIR-V + reflection out. False with `out.error` set.
    // The modules are plain SPIR-V 1.0: the backends compile them with their
    // usual fast math (the translation guards the one function that gave
    // NaN under it, pow at zero — ShaderPackGlsl::Vulkanize).
    bool Compile(const std::string& vertexGlsl, const std::string& fragmentGlsl,
                 const std::string& label, Program& out);

    // The Metal translation of one compiled stage: samplers at texture and
    // sampler index = binding, the uniform block (set 1 binding 0) at
    // `uniformBufferIndex`, the engine's user uniform block (set 3 binding
    // 0 — the terrain section-origin table) at `userUniformIndex`. The
    // entry point is `main0`. False with `error` set.
    bool ToMetal(const std::vector<uint32_t>& spirv, bool vertex, uint32_t uniformBufferIndex,
                 uint32_t userUniformIndex, std::string& msl, std::string& error);

} // namespace Shaders::PackCompiler
