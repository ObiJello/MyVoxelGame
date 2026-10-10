// File: src/client/renderer/backend/metal/MetalPipelines.mm
//
// The Metal backend's shaders, pipeline and depth-stencil states, draws,
// in-pass clears, the pipeline manifest, and Improved Transparency.
#ifdef HAS_METAL

#import "MetalBackend.hpp"
#import "MetalBindings.hpp"

#include "common/core/Config.hpp"   // GAME_VERSION stamps the pipeline manifest
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "platform/GameDirectory.hpp"
#include "client/shader/PackCompiler.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace Render {

    namespace {
        MTLCompareFunction ToMtl(CompareOp op) {
            switch (op) {
                case CompareOp::Never:        return MTLCompareFunctionNever;
                case CompareOp::Less:         return MTLCompareFunctionLess;
                case CompareOp::Equal:        return MTLCompareFunctionEqual;
                case CompareOp::LessEqual:    return MTLCompareFunctionLessEqual;
                case CompareOp::Greater:      return MTLCompareFunctionGreater;
                case CompareOp::NotEqual:     return MTLCompareFunctionNotEqual;
                case CompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
                case CompareOp::Always:       return MTLCompareFunctionAlways;
            }
            return MTLCompareFunctionLessEqual;
        }
        MTLStencilOperation ToMtl(StencilOp op) {
            switch (op) {
                case StencilOp::Keep:      return MTLStencilOperationKeep;
                case StencilOp::Zero:      return MTLStencilOperationZero;
                case StencilOp::Replace:   return MTLStencilOperationReplace;
                case StencilOp::IncrClamp: return MTLStencilOperationIncrementClamp;
                case StencilOp::DecrClamp: return MTLStencilOperationDecrementClamp;
                case StencilOp::Invert:    return MTLStencilOperationInvert;
                case StencilOp::IncrWrap:  return MTLStencilOperationIncrementWrap;
                case StencilOp::DecrWrap:  return MTLStencilOperationDecrementWrap;
            }
            return MTLStencilOperationKeep;
        }
        MTLBlendFactor ToMtl(BlendFactor f) {
            switch (f) {
                case BlendFactor::Zero:             return MTLBlendFactorZero;
                case BlendFactor::One:              return MTLBlendFactorOne;
                case BlendFactor::SrcColor:         return MTLBlendFactorSourceColor;
                case BlendFactor::OneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
                case BlendFactor::DstColor:         return MTLBlendFactorDestinationColor;
                case BlendFactor::OneMinusDstColor: return MTLBlendFactorOneMinusDestinationColor;
                case BlendFactor::SrcAlpha:         return MTLBlendFactorSourceAlpha;
                case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
                case BlendFactor::DstAlpha:         return MTLBlendFactorDestinationAlpha;
                case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
            }
            return MTLBlendFactorOne;
        }
        MTLVertexFormat ToMtl(const VertexAttribute& a) {
            if (a.type == AttribType::Float) {
                switch (a.componentCount) {
                    case 1: return MTLVertexFormatFloat;
                    case 2: return MTLVertexFormatFloat2;
                    case 3: return MTLVertexFormatFloat3;
                    case 4: return MTLVertexFormatFloat4;
                }
            } else if (a.type == AttribType::UShort) {
                if (a.componentCount == 2) return a.normalized ? MTLVertexFormatUShort2Normalized : MTLVertexFormatUShort2;
                if (a.componentCount == 4) return a.normalized ? MTLVertexFormatUShort4Normalized : MTLVertexFormatUShort4;
            } else if (a.componentCount == 4) {
                return a.normalized ? MTLVertexFormatUChar4Normalized : MTLVertexFormatUChar4;
            }
            return MTLVertexFormatInvalid;
        }

        // The attachment configurations a pipeline is built for (the pass's
        // Pass::config): the frame / render targets, and Improved
        // Transparency's three pass shapes (VKBackend::OitPassConfig + 1).
        struct AttachmentConfig {
            uint32_t count;
            MTLPixelFormat formats[8];
        };
        AttachmentConfig AttachmentsFor(uint8_t config) {
            switch (config) {
                case 1:  return {1, {MTLPixelFormatRGBA32Float}};                            // depth bounds (MAX)
                case 2:  return {2, {MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float}}; // transmittance
                case 3:  return {1, {MTLPixelFormatRGBA16Float}};                            // accumulate
                default: return {1, {MTLPixelFormatBGRA8Unorm}};                             // the frame, targets (4: no depth)
            }
        }
        // Configs 4 (MetalBackend::kConfigTargetNoDepth) and 6 have no
        // depth/stencil attachment: their pipelines bake no depth format,
        // their draws no test. Configs 5 / 6 are targets over the caller's
        // textures, whose formats the pass carries (Pass::formats).
        bool ConfigHasDepth(uint8_t config) { return config != 4 && config != 6; }
        // Configs 1..3 are Improved Transparency's passes (MC's OIT snippets:
        // One/One blends, pipelines that live and die with the option). A
        // depth-less target is an ordinary pass: `config != 0` is NOT the
        // test for OIT — it gave the rain target additive blending once
        // (white streaks wherever two columns overlapped, Metal only).
        bool ConfigIsOit(uint8_t config) { return config >= 1 && config <= 3; }

        // Pipeline manifest (VKBackend's): "<vert>\t<frag>\t<config>\t<hex PipelineState>".
        std::string HexOf(const void* data, size_t n) {
            static const char* d = "0123456789abcdef";
            const auto* b = static_cast<const unsigned char*>(data);
            std::string out;
            out.reserve(n * 2);
            for (size_t i = 0; i < n; ++i) { out.push_back(d[b[i] >> 4]); out.push_back(d[b[i] & 15]); }
            return out;
        }
        bool UnhexInto(const std::string& hex, void* data, size_t n) {
            if (hex.size() != n * 2) return false;
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            auto* b = static_cast<unsigned char*>(data);
            for (size_t i = 0; i < n; ++i) {
                const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
                if (hi < 0 || lo < 0) return false;
                b[i] = static_cast<unsigned char>((hi << 4) | lo);
            }
            return true;
        }
        std::string ManifestHeader() {
            return "mtlpm1 " + std::to_string(sizeof(PipelineState)) + " " + GAME_VERSION;
        }
        std::set<std::string> ReadManifest(const std::string& file) {
            std::set<std::string> lines;
            std::ifstream in(file);
            std::string header;
            if (!in.is_open() || !std::getline(in, header) || header != ManifestHeader()) return lines;
            std::string line;
            while (std::getline(in, line)) if (!line.empty()) lines.insert(line);
            return lines;
        }

        // The backend's own MSL: the in-pass clear (a full-rect triangle at a
        // given depth, one or two colour outputs).
        const char* kInternalSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct ClearParams { float4 color; float depth; };
struct ClearOut { float4 position [[position]]; };
vertex ClearOut clear_vertex(uint vid [[vertex_id]], constant ClearParams& p [[buffer(0)]]) {
    float2 xy = float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0;
    ClearOut o;
    o.position = float4(xy, p.depth, 1.0);
    return o;
}
fragment float4 clear_fragment(ClearOut in [[stage_in]], constant ClearParams& p [[buffer(0)]]) {
    return p.color;
}
)MSL";
    }

    // ========================================================================
    // SHADERS
    // ========================================================================

    bool MetalBackend::CreateInternalPipelines() {
        @autoreleasepool {
            NSError* error = nil;
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion2_4;
            // The clear's multi-target twins (clear_fragmentN writes N colour
            // outputs): a shader pack's gbuffers target has up to eight.
            std::string source = kInternalSource;
            for (int n = 2; n <= 8; ++n) {
                source += "struct ClearOut" + std::to_string(n) + " {";
                for (int i = 0; i < n; ++i) source += " float4 c" + std::to_string(i) + " [[color(" + std::to_string(i) + ")]];";
                source += " };\nfragment ClearOut" + std::to_string(n) + " clear_fragment" + std::to_string(n) +
                          "(ClearOut in [[stage_in]], constant ClearParams& p [[buffer(0)]]) { ClearOut" + std::to_string(n) + " o;";
                for (int i = 0; i < n; ++i) source += " o.c" + std::to_string(i) + " = p.color;";
                source += " return o; }\n";
            }
            m_internalLibrary = [m_device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
            if (!m_internalLibrary) {
                Log::Error("MetalBackend: internal shaders failed to compile: %s",
                           error ? error.localizedDescription.UTF8String : "unknown");
                return false;
            }
            m_internalLibrary.label = @"Clear quad";
            return true;
        }
    }

    id<MTLFunction> MetalBackend::LoadFunction(const std::string& glslPath, const char* stage, id<MTLLibrary>* outLibrary) {
        // shaders/<name>.<stage> -> shaders/metal/<name>_vk.<stage>.metal,
        // the MSL tools/gen_metal_shaders.py made from <name>_vk.<stage>.spv.
        const std::string ext = std::string(".") + stage;
        const size_t pos = glslPath.rfind(ext);
        if (pos == std::string::npos) return nil;
        const std::string stem = glslPath.substr(0, pos);
        const size_t slash = stem.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : stem.substr(0, slash + 1);
        const std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);
        // A shader with a half-precision variant (CMake's VK_F16_SHADERS:
        // <name>_f16_vk.frag.spv, TERRAIN_F16) is loaded as that variant —
        // every Apple GPU runs half natively; the plain one is the fallback.
        std::string path = dir + "metal/" + name + "_f16_vk." + stage + ".metal";
        // The entry point tools/gen_metal_shaders.py gave it (entry_point()).
        NSString* entry = [NSString stringWithFormat:@"%s_f16_vk_%s", name.c_str(), stage];
        if (m_shaderLibrary) {
            if (id<MTLFunction> f = [m_shaderLibrary newFunctionWithName:entry]) {
                if (outLibrary) *outLibrary = m_shaderLibrary;
                return f;
            }
        }
        if (!std::filesystem::exists(path)) {
            path = dir + "metal/" + name + "_vk." + stage + ".metal";
            entry = [NSString stringWithFormat:@"%s_vk_%s", name.c_str(), stage];
            if (m_shaderLibrary) {
                if (id<MTLFunction> f = [m_shaderLibrary newFunctionWithName:entry]) {
                    if (outLibrary) *outLibrary = m_shaderLibrary;
                    return f;
                }
            }
        }

        id<MTLLibrary> library = nil;
        auto it = m_libraries.find(path);
        if (it != m_libraries.end()) {
            library = it->second;
        } else {
            PROFILE_ZONE_N("Mtl.CompileLibrary");
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) return nil;
            std::stringstream ss;
            ss << in.rdbuf();
            @autoreleasepool {
                NSError* error = nil;
                MTLCompileOptions* options = [MTLCompileOptions new];
                options.languageVersion = MTLLanguageVersion2_4;
                NSString* source = [NSString stringWithUTF8String:ss.str().c_str()];
                library = [m_device newLibraryWithSource:source options:options error:&error];
                if (!library) {
                    Log::Error("MetalBackend: %s failed to compile: %s", path.c_str(),
                               error ? error.localizedDescription.UTF8String : "unknown");
                    return nil;
                }
            }
            m_libraries[path] = library;
        }
        if (outLibrary) *outLibrary = library;
        return [library newFunctionWithName:entry];
    }

    ShaderHandle MetalBackend::CreateShader(const std::string& /*vertexSource*/, const std::string& /*fragmentSource*/) {
        Log::Error("MetalBackend: CreateShader from GLSL source not supported - use the shader files");
        return INVALID_SHADER;
    }

    ShaderHandle MetalBackend::CreateShaderFromFiles(const std::string& vertexPath, const std::string& fragmentPath) {
        if (!m_device) return INVALID_SHADER;
        ShaderInfo info;
        id<MTLLibrary> vertLibrary = nil, fragLibrary = nil;
        info.vertex = LoadFunction(vertexPath, "vert", &vertLibrary);
        info.fragment = LoadFunction(fragmentPath, "frag", &fragLibrary);
        info.vertLibrary = vertLibrary;
        info.fragLibrary = fragLibrary;
        if (!info.vertex || !info.fragment) {
            Log::Error("MetalBackend: Failed to load Metal shaders for %s, %s", vertexPath.c_str(), fragmentPath.c_str());
            return INVALID_SHADER;
        }
        info.vertPath = vertexPath;
        info.fragPath = fragmentPath;
        const uint32_t handle = AllocHandle();
        m_shaders[handle] = std::move(info);
        ++m_memStats.shaderCount;
        Log::Info("MetalBackend: Loaded Metal shaders: %s + %s", vertexPath.c_str(), fragmentPath.c_str());
        return handle;
    }

    ShaderHandle MetalBackend::CreateShaderFromFilesPortal(const std::string& vertexPath, const std::string& fragmentPath) {
        const ShaderHandle h = CreateShaderFromFiles(vertexPath, fragmentPath);
        if (h != INVALID_SHADER) m_shaders[h].layoutType = 1;   // reads the Common block
        return h;
    }

    ShaderHandle MetalBackend::CreateOitShaderFromFiles(const std::string& vertexPath, const std::string& fragmentPath) {
        const ShaderHandle h = CreateShaderFromFiles(vertexPath, fragmentPath);
        if (h != INVALID_SHADER) m_shaders[h].layoutType = 4;   // block + the OIT set
        return h;
    }

    void MetalBackend::RegisterShaderVertexLayout(ShaderHandle shader, const VertexLayout& layout) {
        auto it = m_shaders.find(shader);
        if (it != m_shaders.end()) it->second.vertexLayout = layout;
    }

    void MetalBackend::RegisterShaderInstanceLayout(ShaderHandle shader, const VertexLayout& layout) {
        auto it = m_shaders.find(shader);
        if (it != m_shaders.end()) it->second.instanceLayout = layout;
    }

    void MetalBackend::SetShaderIgnoresCommonMatrices(ShaderHandle shader) {
        auto it = m_shaders.find(shader);
        if (it != m_shaders.end()) it->second.ignoresCommonMatrices = true;
    }

    void MetalBackend::DestroyShader(ShaderHandle handle) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        if (m_boundShader == handle) {
            m_boundShader = INVALID_SHADER;
            m_boundShaderInfo = nullptr;
        }
        for (auto pit = m_pipelines.begin(); pit != m_pipelines.end();) {
            if (pit->second.shader == handle) pit = m_pipelines.erase(pit);
            else ++pit;
        }
        if (m_memStats.shaderCount > 0) --m_memStats.shaderCount;
        m_shaders.erase(it);
        m_drawStateDirty = true;   // its pipelines are gone
    }

    void MetalBackend::BindShader(ShaderHandle handle) {
        // Improved Transparency: inside an OIT stage the engine shader's
        // variant draws instead — or nothing, when it has none.
        if (m_oitStage != OitStage::None) {
            const ShaderHandle variant = OitVariantFor(handle);
            m_oitSkipDraw = variant == INVALID_SHADER;
            if (!m_oitSkipDraw) handle = variant;
        }
        // Override mode (the shader pack pipeline): the pack's program and
        // its render target in place of the engine's, or the engine's program
        // into the default target — the OpenGL backend's rule.
        RenderTargetHandle target = INVALID_RENDER_TARGET;
        ShaderHandle layoutSource = INVALID_SHADER;
        if (m_overrideMode) {
            target = m_overrideDefaultTarget;
            auto ov = m_shaderOverrides.find(handle);
            if (ov != m_shaderOverrides.end()) {
                if (ov->second.shader != INVALID_SHADER && ov->second.shader != handle) {
                    layoutSource = handle;   // the pack program draws this shader's meshes
                    handle = ov->second.shader;
                }
                if (ov->second.target != INVALID_RENDER_TARGET) target = ov->second.target;
            }
        }
        if (m_packLayoutSource != layoutSource) m_drawStateDirty = true;
        m_packLayoutSource = layoutSource;
        m_boundShader = handle;
        auto it = m_shaders.find(handle);
        m_boundShaderInfo = it != m_shaders.end() ? &it->second : nullptr;
        if (m_boundShaderInfo && m_boundShaderInfo->layoutType == kLayoutPack) m_packFrame = true;
        m_drawStateDirty = true;
        if (target != INVALID_RENDER_TARGET) BindRenderTarget(target);
    }

    // ── pack programs ────────────────────────────────────────────────────

    ShaderHandle MetalBackend::CreatePackShader(const PackShaderDesc& desc) {
        if (!m_device) return INVALID_SHADER;
        PROFILE_ZONE_N("Mtl.CreatePackShader");
        @autoreleasepool {
            MTLCompileOptions* options = [MTLCompileOptions new];
            // Fast math, as for every shader (precise functions cost the
            // pack's deferred passes 5x on 2026-10-09); the translation
            // guards pow at zero, the one NaN fast math gave a pack.
            options.fastMathEnabled = YES;
            // OBEY_PACK_MSL=<flags>: compile-side experiments for a pack's
            // programs, for A/B against the default — `noinline` drops
            // SPIRV-Cross's always_inline so the Metal compiler decides
            // (register pressure), `size` the size optimisation level,
            // `mathapi` the macOS 15 math-mode API in place of fastMathEnabled.
            static const std::string s_mslFlags = std::getenv("OBEY_PACK_MSL") ? std::getenv("OBEY_PACK_MSL") : "";
            const bool noInline = s_mslFlags.find("noinline") != std::string::npos;
            if (s_mslFlags.find("size") != std::string::npos) options.optimizationLevel = MTLLibraryOptimizationLevelSize;
            if (s_mslFlags.find("mathapi") != std::string::npos) {
                if (@available(macOS 15.0, *)) {
                    options.mathMode = MTLMathModeFast;
                    options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsFast;
                }
            }
            NSError* error = nil;
            // A stage's library and entry function from its MSL; nil on
            // failure (logged with the stage).
            auto compileLibrary = [&](const std::string& mslIn, const char* stage) -> id<MTLLibrary> {
                std::string msl = mslIn;
                if (noInline) {
                    static const std::string kInline = "static inline __attribute__((always_inline))";
                    for (size_t at = msl.find(kInline); at != std::string::npos; at = msl.find(kInline, at)) {
                        msl.replace(at, kInline.size(), "static");
                    }
                }
                id<MTLLibrary> library = [m_device newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()] options:options error:&error];
                if (!library) {
                    Log::Error("MetalBackend: pack program %s %s: %s", desc.label.c_str(), stage,
                               error ? error.localizedDescription.UTF8String : "no library");
                }
                return library;
            };
            auto entryFunction = [&](id<MTLLibrary> library, const std::string& entry, const char* stage) -> id<MTLFunction> {
                id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:entry.c_str()]];
                if (!function) {
                    Log::Error("MetalBackend: pack program %s %s: no entry point %s", desc.label.c_str(), stage, entry.c_str());
                    return nil;
                }
                function.label = [NSString stringWithFormat:@"%s.%s", desc.label.c_str(), stage];
                return function;
            };
            // The MSL: the caller's, or translated here from the SPIR-V with
            // this backend's indices (the pack block, the user block, the
            // texture slots).
            std::string vertexMsl = desc.vertexMsl, fragmentMsl = desc.fragmentMsl, translateError;
            if (vertexMsl.empty() &&
                !Shaders::PackCompiler::ToMetal(desc.vertexSpirv, true, MetalBindings::kPackUniforms,
                                                MetalBindings::kUserUniform, vertexMsl, translateError)) {
                Log::Error("MetalBackend: pack program %s: %s", desc.label.c_str(), translateError.c_str());
                return INVALID_SHADER;
            }
            if (fragmentMsl.empty() &&
                !Shaders::PackCompiler::ToMetal(desc.fragmentSpirv, false, MetalBindings::kPackUniforms,
                                                MetalBindings::kUserUniform, fragmentMsl, translateError)) {
                Log::Error("MetalBackend: pack program %s: %s", desc.label.c_str(), translateError.c_str());
                return INVALID_SHADER;
            }
            if (const char* dump = std::getenv("OBEY_PACK_DUMP"); dump && std::strcmp(dump, "msl") == 0) {
                // Next to the pipeline's translated GLSL (ShaderPipeline::DumpSource).
                std::string gameDir = Platform::g_gameDirectory.GetGameDirectory();
                if (gameDir.empty()) gameDir = Platform::GameDirectory::GetDefaultGameDirectory();
                const std::filesystem::path dir = std::filesystem::path(gameDir) / "shaderpacks" / ".translated";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::ofstream(dir / (desc.label + ".vsh.metal")) << vertexMsl;
                std::ofstream(dir / (desc.label + ".fsh.metal")) << fragmentMsl;
            }
            ShaderInfo info;
            info.vertLibrary = compileLibrary(vertexMsl, "vsh");
            if (!info.vertLibrary) return INVALID_SHADER;
            info.vertex = entryFunction(info.vertLibrary, desc.vertexEntry, "vsh");
            if (!info.vertex) return INVALID_SHADER;
            info.fragLibrary = compileLibrary(fragmentMsl, "fsh");
            if (!info.fragLibrary) return INVALID_SHADER;
            info.fragment = entryFunction(info.fragLibrary, desc.fragmentEntry, "fsh");
            if (!info.fragment) return INVALID_SHADER;
            info.vertPath = "pack:" + desc.label + ".vsh";
            info.fragPath = "pack:" + desc.label + ".fsh";
            info.layoutType = kLayoutPack;
            info.packBlockSize = desc.uniformBlockSize;
            info.packBlock.assign(desc.uniformBlockSize, 0);
            for (const PackUniformDesc& u : desc.uniforms) info.packUniforms[u.name] = u;
            info.packSampler2D = desc.sampler2DSlots;
            const uint32_t handle = AllocHandle();
            m_shaders[handle] = std::move(info);
            ++m_memStats.shaderCount;
            return handle;
        }
    }

    bool MetalBackend::SetPackUniform(ShaderHandle shader, const std::string& name, const void* data, size_t bytes) {
        // The engine sets its uniforms on its own handles; under an override
        // they land in the pack program drawn in its place.
        if (m_overrideMode) {
            auto ov = m_shaderOverrides.find(shader);
            if (ov != m_shaderOverrides.end() && ov->second.shader != INVALID_SHADER) shader = ov->second.shader;
        }
        auto it = m_shaders.find(shader);
        if (it == m_shaders.end() || it->second.layoutType != kLayoutPack) return false;
        ShaderInfo& info = it->second;
        // `name[i]`: element i of an array member.
        size_t element = 0;
        std::string base;
        const std::string* key = &name;
        if (!name.empty() && name.back() == ']') {
            const size_t br = name.find('[');
            if (br == std::string::npos) return true;
            base = name.substr(0, br);
            element = static_cast<size_t>(std::atoi(name.c_str() + br + 1));
            key = &base;
        }
        auto u = info.packUniforms.find(*key);
        if (u == info.packUniforms.end()) return true;   // not a uniform of this program (a sampler's unit, an unused one)
        const PackUniformDesc& d = u->second;
        if (element >= std::max<uint32_t>(1, d.arrayLength)) return true;
        const uint32_t elementSize = d.arrayLength > 1 ? d.arrayStride : d.size;
        const uint32_t offset = d.offset + static_cast<uint32_t>(element) * d.arrayStride;
        if (offset + elementSize > info.packBlock.size()) return true;
        uint8_t* dst = info.packBlock.data() + offset;
        const uint8_t* src = static_cast<const uint8_t*>(data);
        // No depth remap here (SpirvUniforms does one for the engine's own
        // uMVP): a pack program's matrices stay GL-style, and its translated
        // vertex stage remaps clip z itself (ShaderPackGlsl::Vulkanize).
        // OBEY_PACK_DUMP=block[:<program substring>]: the first writes, with values.
        {
            static const char* s_dump = std::getenv("OBEY_PACK_DUMP");
            static const bool s_dumpBlock = s_dump && std::strncmp(s_dump, "block", 5) == 0;
            static const std::string s_filter = s_dumpBlock && s_dump[5] == ':' ? std::string(s_dump + 6) : std::string();
            static int s_logged = 0;
            if (s_dumpBlock && s_logged < 3000 && (s_filter.empty() || info.vertPath.find(s_filter) != std::string::npos)) {
                ++s_logged;
                const float* f = reinterpret_cast<const float*>(src);
                const size_t n = std::min<size_t>(bytes / 4, 4);
                std::string vals;
                for (size_t i = 0; i < n; ++i) vals += (i ? ", " : "") + std::to_string(f[i]);
                Log::Info("[PackBlock] %s %s[%zu] kind %d offset %u size %u: %s", info.vertPath.c_str(), key->c_str(), element,
                          static_cast<int>(d.kind), offset, elementSize, vals.c_str());
            }
        }
        switch (d.kind) {
            case PackUniformKind::Mat3:
                // Three std140 columns, 16 bytes apart, from glm's packed 36.
                for (int c = 0; c < 3 && bytes >= 36; ++c) std::memcpy(dst + c * 16, src + c * 12, 12);
                break;
            case PackUniformKind::Bool:
            case PackUniformKind::Int:
            case PackUniformKind::Float:
                std::memcpy(dst, src, std::min<size_t>(bytes, 4));
                break;
            default:
                std::memcpy(dst, src, std::min<size_t>(bytes, elementSize));
                break;
        }
        info.packDirty = true;
        return true;
    }

    void MetalBackend::SetShaderOverrideMode(bool on, RenderTargetHandle defaultTarget) {
        m_overrideMode = on;
        m_overrideDefaultTarget = on ? defaultTarget : INVALID_RENDER_TARGET;
        m_drawStateDirty = true;
    }

    void MetalBackend::SetShaderOverride(ShaderHandle engine, ShaderHandle pack, RenderTargetHandle target) {
        if (engine == INVALID_SHADER) return;
        if (pack == INVALID_SHADER && target == INVALID_RENDER_TARGET) { m_shaderOverrides.erase(engine); return; }
        m_shaderOverrides[engine] = {pack, target};
    }

    std::vector<ShaderHandle> MetalBackend::FindShadersBySource(
        const std::function<bool(const std::string&, const std::string&)>& match) {
        // The engine's shaders are loaded by their GLSL paths; the sources
        // behind them are what the pack pipeline's predicates read.
        auto readFile = [](const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) return std::string();
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        std::vector<ShaderHandle> out;
        for (auto& [handle, info] : m_shaders) {
            if (info.layoutType == kLayoutPack || info.vertPath.empty()) continue;
            if (!info.sourcesLoaded) {
                info.vertexSource = readFile(info.vertPath);
                info.fragmentSource = readFile(info.fragPath);
                info.sourcesLoaded = true;
            }
            if (match(info.vertexSource, info.fragmentSource)) out.push_back(handle);
        }
        return out;
    }

    void MetalBackend::ClearShaderOverrides() {
        m_shaderOverrides.clear();
        m_overrideMode = false;
        m_overrideDefaultTarget = INVALID_RENDER_TARGET;
    }

    // ========================================================================
    // PIPELINES
    // ========================================================================

    MetalBackend::PipelineKeyT MetalBackend::PipelineKey(const PipelineState& state, ShaderHandle shader, uint8_t config) const {
        // What a Metal pipeline bakes: the shader (its vertex input with it),
        // the attachments' formats and the blend. Depth, stencil, cull,
        // winding, fill, bias and topology are encoder state.
        uint64_t key = shader;
        key |= static_cast<uint64_t>(config & 7u) << 32;
        // A target over the caller's textures: its formats are the pass's,
        // so the key carries them (PrepareDraw keys on the current pass).
        if (ConfigFromPass(config)) key |= static_cast<uint64_t>(m_pass.attachmentsKey & 0x3FFFFu) << 46;
        key |= static_cast<uint64_t>(state.blendEnabled ? 1 : 0) << 35;
        key |= static_cast<uint64_t>(state.srcBlendFactor) << 36;
        key |= static_cast<uint64_t>(state.dstBlendFactor) << 40;
        key |= static_cast<uint64_t>(state.blendAlphaLikeColor ? 1 : 0) << 44;
        key |= static_cast<uint64_t>(state.colorWriteEnabled ? 1 : 0) << 45;
        PipelineKeyT k;
        k.a = key;
        // A pack program under an override: the engine shader's layout.
        if (m_packLayoutSource != INVALID_SHADER && shader == m_boundShader) k.b = m_packLayoutSource;
        return k;
    }

    id<MTLRenderPipelineState> MetalBackend::PipelineFor(const PipelineState& state, ShaderHandle shader, uint8_t config) {
        const PipelineKeyT key = PipelineKey(state, shader, config);
        auto it = m_pipelines.find(key);
        if (it != m_pipelines.end()) return it->second.pipeline;
        auto sit = m_shaders.find(shader);
        if (sit == m_shaders.end()) return nil;
        const ShaderInfo& info = sit->second;
        // The vertex input a pack program draws with: the overridden engine
        // shader's (PipelineKeyT::b), else its own registration.
        const ShaderInfo* layoutInfo = &info;
        if (key.b != 0) {
            auto lit = m_shaders.find(key.b);
            if (lit != m_shaders.end()) layoutInfo = &lit->second;
        }
        // A Metal pipeline compile (the MSL was compiled with the library):
        // mid-frame it is a hitch, which WarmPipelines takes off the frame.
        PROFILE_ZONE_N("Mtl.CreatePipeline");
        @autoreleasepool {
            MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
            desc.vertexFunction = info.vertex;
            desc.fragmentFunction = info.fragment;
            // Named for the Metal debugger: the shader pair and pass config.
            auto stem = [](const std::string& path) {
                const size_t slash = path.find_last_of('/');
                return slash == std::string::npos ? path : path.substr(slash + 1);
            };
            desc.label = [NSString stringWithFormat:@"%s + %s%s", stem(info.vertPath).c_str(),
                                                    stem(info.fragPath).c_str(),
                                                    ConfigIsOit(config) ? [NSString stringWithFormat:@" (OIT %u)", config].UTF8String
                                                                        : ConfigHasDepth(config) ? "" : " (no depth)"];

            // Vertex input: the shader's registered layout, else the 24-byte
            // block layout (pos3f + uv2f + colour4u8) — Vulkan's rule. Only
            // the attributes the function actually reads are described.
            VertexLayout layout = layoutInfo->vertexLayout;
            if (layout.attributes.empty() && !layout.noVertexInput) {
                layout.stride = 24;
                layout.attributes = {{0, 3, 0, false, AttribType::Float},
                                     {1, 2, 12, false, AttribType::Float},
                                     {2, 4, 20, true, AttribType::UByte}};
            }
            NSMutableSet<NSNumber*>* used = [NSMutableSet set];
            for (MTLVertexAttribute* a in info.vertex.vertexAttributes) {
                if (a.active) [used addObject:@(a.attributeIndex)];
            }
            if (used.count > 0) {
                MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
                bool anyVertex = false, anyInstance = false;
                NSMutableSet<NSNumber*>* described = [NSMutableSet set];
                for (const VertexAttribute& a : layout.attributes) {
                    if (![used containsObject:@(a.location)]) continue;
                    vd.attributes[a.location].format = ToMtl(a);
                    vd.attributes[a.location].offset = a.offset;
                    vd.attributes[a.location].bufferIndex = MetalBindings::kVertexStream;
                    [described addObject:@(a.location)];
                    anyVertex = true;
                }
                for (const VertexAttribute& a : layoutInfo->instanceLayout.attributes) {
                    if (![used containsObject:@(a.location)]) continue;
                    vd.attributes[a.location].format = ToMtl(a);
                    vd.attributes[a.location].offset = a.offset;
                    vd.attributes[a.location].bufferIndex = MetalBindings::kInstanceStream;
                    [described addObject:@(a.location)];
                    anyInstance = true;
                }
                if (info.layoutType == kLayoutPack && anyVertex) {
                    // A pack program reading an attribute this layout lacks
                    // (a mesh without colour, say): described anyway, four
                    // bytes at the vertex's start, so the pipeline builds;
                    // OpenGL handed such attributes their default.
                    for (NSNumber* n in used) {
                        if ([described containsObject:n] || n.unsignedIntegerValue >= 31) continue;
                        vd.attributes[n.unsignedIntegerValue].format = MTLVertexFormatUChar4Normalized;
                        vd.attributes[n.unsignedIntegerValue].offset = 0;
                        vd.attributes[n.unsignedIntegerValue].bufferIndex = MetalBindings::kVertexStream;
                    }
                }
                if (anyVertex) {
                    vd.layouts[MetalBindings::kVertexStream].stride = layout.stride;
                    vd.layouts[MetalBindings::kVertexStream].stepFunction = MTLVertexStepFunctionPerVertex;
                }
                if (anyInstance) {
                    vd.layouts[MetalBindings::kInstanceStream].stride = info.instanceLayout.stride;
                    vd.layouts[MetalBindings::kInstanceStream].stepFunction = MTLVertexStepFunctionPerInstance;
                    vd.layouts[MetalBindings::kInstanceStream].stepRate = 1;
                }
                desc.vertexDescriptor = vd;
            }

            AttachmentConfig att = AttachmentsFor(config);
            if (ConfigFromPass(config)) {
                att.count = static_cast<uint32_t>(std::min<size_t>(m_pass.formats.size(), 8));
                for (uint32_t i = 0; i < att.count; ++i) att.formats[i] = m_pass.formats[i];
            }
            const bool oit = ConfigIsOit(config);
            for (uint32_t i = 0; i < att.count; ++i) {
                MTLRenderPipelineColorAttachmentDescriptor* c = desc.colorAttachments[i];
                c.pixelFormat = att.formats[i];
                c.writeMask = state.colorWriteEnabled ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
                c.blendingEnabled = state.blendEnabled ? YES : NO;
                if (oit) {
                    // MC's OIT snippets: One/One on colour and alpha — MAX into
                    // the depth bounds, ADD into the coefficients and the
                    // accumulation — on every attachment.
                    const MTLBlendOperation op = config == 1 ? MTLBlendOperationMax : MTLBlendOperationAdd;
                    c.sourceRGBBlendFactor = c.destinationRGBBlendFactor = MTLBlendFactorOne;
                    c.sourceAlphaBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOne;
                    c.rgbBlendOperation = c.alphaBlendOperation = op;
                } else {
                    c.sourceRGBBlendFactor = ToMtl(state.srcBlendFactor);
                    c.destinationRGBBlendFactor = ToMtl(state.dstBlendFactor);
                    c.rgbBlendOperation = MTLBlendOperationAdd;
                    // Vulkan writes the fragment's own alpha unless the state
                    // asks alpha to blend like colour (what glBlendFunc does).
                    c.sourceAlphaBlendFactor = state.blendAlphaLikeColor ? ToMtl(state.srcBlendFactor) : MTLBlendFactorOne;
                    c.destinationAlphaBlendFactor = state.blendAlphaLikeColor ? ToMtl(state.dstBlendFactor) : MTLBlendFactorZero;
                    c.alphaBlendOperation = MTLBlendOperationAdd;
                }
            }
            desc.depthAttachmentPixelFormat = ConfigHasDepth(config) ? kDepthFormat : MTLPixelFormatInvalid;
            desc.stencilAttachmentPixelFormat = ConfigHasDepth(config) ? kDepthFormat : MTLPixelFormatInvalid;
            desc.rasterSampleCount = 1;

            NSError* error = nil;
            id<MTLRenderPipelineState> pipeline = nil;
            if (m_metal4) {
                // Metal 4's compiler, from the same descriptor's contents:
                // function descriptors (library + name) in place of
                // functions, no depth/stencil formats (the pass's), a blend
                // state per attachment. Metal 3 pipelines would run on the
                // Metal 4 encoders too; this is the path's own.
                if (@available(macOS 26.0, *)) {
                    MTL4RenderPipelineDescriptor* d4 = [MTL4RenderPipelineDescriptor new];
                    MTL4LibraryFunctionDescriptor* vf = [MTL4LibraryFunctionDescriptor new];
                    vf.library = info.vertLibrary;
                    vf.name = info.vertex.name;
                    MTL4LibraryFunctionDescriptor* ff = [MTL4LibraryFunctionDescriptor new];
                    ff.library = info.fragLibrary;
                    ff.name = info.fragment.name;
                    d4.vertexFunctionDescriptor = vf;
                    d4.fragmentFunctionDescriptor = ff;
                    d4.vertexDescriptor = desc.vertexDescriptor;
                    d4.label = desc.label;
                    d4.rasterSampleCount = 1;
                    for (uint32_t i = 0; i < att.count; ++i) {
                        MTLRenderPipelineColorAttachmentDescriptor* c = desc.colorAttachments[i];
                        MTL4RenderPipelineColorAttachmentDescriptor* c4 = d4.colorAttachments[i];
                        c4.pixelFormat = c.pixelFormat;
                        c4.writeMask = c.writeMask;
                        c4.blendingState = c.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
                        c4.sourceRGBBlendFactor = c.sourceRGBBlendFactor;
                        c4.destinationRGBBlendFactor = c.destinationRGBBlendFactor;
                        c4.rgbBlendOperation = c.rgbBlendOperation;
                        c4.sourceAlphaBlendFactor = c.sourceAlphaBlendFactor;
                        c4.destinationAlphaBlendFactor = c.destinationAlphaBlendFactor;
                        c4.alphaBlendOperation = c.alphaBlendOperation;
                    }
                    pipeline = [(id<MTL4Compiler>)m_m4.compiler newRenderPipelineStateWithDescriptor:d4
                                                                                compilerTaskOptions:nil
                                                                                              error:&error];
                }
            } else {
                pipeline = [m_device newRenderPipelineStateWithDescriptor:desc error:&error];
            }
            if (!pipeline) {
                Log::Error("MetalBackend: Failed to create pipeline (shader %s + %s, config %u): %s",
                           info.vertPath.c_str(), info.fragPath.c_str(), static_cast<unsigned>(config),
                           error ? error.localizedDescription.UTF8String : "unknown");
                // Remembered as failed, so a broken pipeline is not retried every draw.
                m_pipelines[key] = PipelineRecord{state, shader, config, nil};
                return nil;
            }
            m_pipelines[key] = PipelineRecord{state, shader, config, pipeline};
            ++m_counters.pipelinesBuilt;   // Mtl/PipelinesBuilt: a mid-tour compile is a hitch to chase
            if (!oit && shader != m_upscaleShader) {
                ++m_pipelinesSinceSave;
                m_lastPipelineFrame = m_frameNumber;
            }
            return pipeline;
        }
    }

    id<MTLDepthStencilState> MetalBackend::DepthStencilFor(const PipelineState& state) {
        // No depth test means no depth writes either (GL and Vulkan alike).
        uint64_t key = 0;
        key |= static_cast<uint64_t>(state.depthTestEnabled ? 1 : 0);
        key |= static_cast<uint64_t>(state.depthTestEnabled && state.depthWriteEnabled ? 1 : 0) << 1;
        key |= static_cast<uint64_t>(state.depthCompareOp) << 2;
        if (state.stencilTestEnabled) {
            key |= 1ull << 5;
            key |= static_cast<uint64_t>(state.stencilCompareOp) << 6;
            key |= static_cast<uint64_t>(state.stencilFailOp) << 9;
            key |= static_cast<uint64_t>(state.stencilDepthFailOp) << 12;
            key |= static_cast<uint64_t>(state.stencilPassOp) << 15;
            key |= static_cast<uint64_t>(state.stencilReadMask & 0xFFu) << 18;
            key |= static_cast<uint64_t>(state.stencilWriteMask & 0xFFu) << 26;
        }
        auto it = m_depthStencilStates.find(key);
        if (it != m_depthStencilStates.end()) return it->second;
        @autoreleasepool {
            MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
            d.depthCompareFunction = state.depthTestEnabled ? ToMtl(state.depthCompareOp) : MTLCompareFunctionAlways;
            d.depthWriteEnabled = state.depthTestEnabled && state.depthWriteEnabled;
            d.label = [NSString stringWithFormat:@"Depth %s%s%s", state.depthTestEnabled ? "test" : "off",
                                                 d.depthWriteEnabled ? " write" : "", state.stencilTestEnabled ? " stencil" : ""];
            if (state.stencilTestEnabled) {
                MTLStencilDescriptor* s = [MTLStencilDescriptor new];
                s.stencilCompareFunction = ToMtl(state.stencilCompareOp);
                s.stencilFailureOperation = ToMtl(state.stencilFailOp);
                s.depthFailureOperation = ToMtl(state.stencilDepthFailOp);
                s.depthStencilPassOperation = ToMtl(state.stencilPassOp);
                s.readMask = state.stencilReadMask & 0xFFu;
                s.writeMask = state.stencilWriteMask & 0xFFu;
                d.frontFaceStencil = s;
                d.backFaceStencil = s;
            }
            id<MTLDepthStencilState> ds = [m_device newDepthStencilStateWithDescriptor:d];
            m_depthStencilStates[key] = ds;
            return ds;
        }
    }

    void MetalBackend::WarmPipelines() {
        if (m_pipelinesWarmed || !m_device) return;
        m_pipelinesWarmed = true;
        {
            std::string gameDir = Platform::g_gameDirectory.GetGameDirectory();
            if (gameDir.empty()) gameDir = Platform::GameDirectory::GetDefaultGameDirectory();
            if (!gameDir.empty()) m_manifestFile = gameDir + "/cache/mtl_pipeline_manifest.txt";
        }
        if (m_manifestFile.empty()) return;
        PROFILE_ZONE_N("Mtl.WarmPipelines");
        const auto t0 = std::chrono::steady_clock::now();
        const std::set<std::string> lines = ReadManifest(m_manifestFile);
        if (lines.empty()) {
            Log::Info("MetalBackend: no pipeline manifest - first frames build pipelines lazily");
            return;
        }
        std::unordered_map<std::string, ShaderHandle> byPath;
        for (const auto& [handle, info] : m_shaders) byPath[info.vertPath + "\t" + info.fragPath] = handle;
        size_t built = 0, unknown = 0;
        for (const std::string& line : lines) {
            // vert \t frag \t config \t state
            const size_t t1 = line.find('\t');
            const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            const size_t t3 = t2 == std::string::npos ? t2 : line.find('\t', t2 + 1);
            if (t3 == std::string::npos) continue;
            auto sh = byPath.find(line.substr(0, t2));
            if (sh == byPath.end()) { ++unknown; continue; }
            const uint8_t config = static_cast<uint8_t>(std::atoi(line.substr(t2 + 1, t3 - t2 - 1).c_str()));
            PipelineState state;
            if (!UnhexInto(line.substr(t3 + 1), &state, sizeof(PipelineState))) continue;
            const size_t before = m_pipelines.size();
            PipelineFor(state, sh->second, config);
            if (m_pipelines.size() > before) ++built;
        }
        m_pipelinesSinceSave = 0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Log::Info("MetalBackend: warmed %zu pipelines in %.0f ms from %zu manifest entries (%zu for shaders not loaded)",
                  built, ms, lines.size(), unknown);
    }

    void MetalBackend::SaveManifest(bool synchronous) {
        if (m_manifestFile.empty()) {
            std::string gameDir = Platform::g_gameDirectory.GetGameDirectory();
            if (gameDir.empty()) gameDir = Platform::GameDirectory::GetDefaultGameDirectory();
            if (gameDir.empty()) return;
            m_manifestFile = gameDir + "/cache/mtl_pipeline_manifest.txt";
        }
        m_pipelinesSinceSave = 0;
        std::vector<std::string> lines;
        for (const auto& [key, rec] : m_pipelines) {
            if (!rec.pipeline || ConfigIsOit(rec.config)) continue;   // OIT passes' pipelines live with the option
            if (ConfigFromPass(rec.config)) continue;                   // a shader pack's targets: their formats are the pack's
            auto it = m_shaders.find(rec.shader);
            if (it == m_shaders.end() || it->second.vertPath.empty() || it->second.layoutType >= 3) continue;
            // The frame's (0) and the depth-less targets' (4) pipelines.
            lines.push_back(it->second.vertPath + "\t" + it->second.fragPath + "\t" +
                            std::to_string(rec.config) + "\t" + HexOf(&rec.state, sizeof(PipelineState)));
        }
        const std::string file = m_manifestFile;
        auto write = [lines = std::move(lines), file]() {
            // Union with what this version already wrote — a session that
            // never visits the End must not forget the End's pipelines.
            std::set<std::string> all = ReadManifest(file);
            all.insert(lines.begin(), lines.end());
            std::error_code ec;
            const std::filesystem::path path(file);
            std::filesystem::create_directories(path.parent_path(), ec);
            const std::filesystem::path tmp = path.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out.is_open()) return;
                out << ManifestHeader() << "\n";
                for (const auto& l : all) out << l << "\n";
            }
            std::filesystem::rename(tmp, path, ec);
        };
        if (m_manifestWriter.joinable()) m_manifestWriter.join();
        if (synchronous) {
            write();
        } else {
            m_manifestWriter = std::thread([w = std::move(write)]() mutable {
                PROFILE_THREAD("PipelineManifestWriter");
                w();
            });
        }
    }

    // ========================================================================
    // PIPELINE STATE
    // ========================================================================

    void MetalBackend::SetStencilOverride(bool enabled, CompareOp compareOp, StencilOp passOp, uint32_t reference,
                                          uint32_t readMask, uint32_t writeMask) {
        m_stencilOverride = {enabled, compareOp, passOp, reference, readMask, writeMask};
    }

    void MetalBackend::SetPipelineState(const PipelineState& requested) {
        // The portal renderer's stencil override, the mirrored view's front
        // face and Improved Transparency's stage, spliced in exactly as
        // VKBackend::SetPipelineState does.
        PipelineState state = requested;
        if (m_stencilOverride.enabled) {
            state.stencilTestEnabled = true;
            state.stencilCompareOp = m_stencilOverride.compareOp;
            state.stencilFailOp = StencilOp::Keep;
            state.stencilDepthFailOp = StencilOp::Keep;
            state.stencilPassOp = m_stencilOverride.passOp;
            state.stencilReference = m_stencilOverride.reference;
            state.stencilReadMask = m_stencilOverride.readMask;
            state.stencilWriteMask = m_stencilOverride.writeMask;
        }
        // A mirrored view flips every triangle's screen winding: flip the
        // front-face rule (not the cull mode), so gl_FrontFacing keeps
        // meaning the geometric front.
        if (m_cullInvert) {
            state.frontFace = state.frontFace == FrontFace::CounterClockwise ? FrontFace::Clockwise
                                                                             : FrontFace::CounterClockwise;
        }
        m_requestedState = requested;
        if (m_oitStage != OitStage::None) {
            state.depthTestEnabled = true;
            state.depthCompareOp = CompareOp::LessEqual;
            state.depthWriteEnabled = m_oitStage == OitStage::DepthBounds && m_oitDbWritesDepth;
            state.blendEnabled = true;
            state.srcBlendFactor = BlendFactor::One;
            state.dstBlendFactor = BlendFactor::One;
            state.colorWriteEnabled = true;
        }
        m_state = state;
        m_drawStateDirty = true;
    }

    // ========================================================================
    // DRAWING
    // ========================================================================

    MTLPrimitiveType MetalBackend::ToPrimitive(PrimitiveType p) {
        switch (p) {
            case PrimitiveType::Lines:         return MTLPrimitiveTypeLine;
            case PrimitiveType::LineStrip:     return MTLPrimitiveTypeLineStrip;
            case PrimitiveType::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
            default:                           return MTLPrimitiveTypeTriangle;
        }
    }

    void MetalBackend::BindVertexStream(uint32_t index, id<MTLBuffer> buffer, NSUInteger offset) {
        if (m_cache.vertexBuffers[index] == buffer) {
            if (m_cache.vertexOffsets[index] != offset) {
                if (m_metal4) M4BindBuffer(false, index, buffer, offset);
                else [m_encoder setVertexBufferOffset:offset atIndex:index];
                m_cache.vertexOffsets[index] = offset;
            }
            return;
        }
        if (m_metal4) M4BindBuffer(false, index, buffer, offset);
        else [m_encoder setVertexBuffer:buffer offset:offset atIndex:index];
        m_cache.vertexBuffers[index] = buffer;
        m_cache.vertexOffsets[index] = offset;
    }

    void MetalBackend::BindUniforms() {
        id enc = m_encoder;
        // Push constants: 128 bytes, inline in the command stream (Metal 4:
        // a ring window, bound through both argument tables).
        if (!m_cache.pushValid || std::memcmp(&m_cache.push, &m_uniforms.push, sizeof(PushConstantBlock)) != 0) {
            if (m_metal4) {
                size_t offset = 0;
                id<MTLBuffer> ring = RingAllocate(sizeof(PushConstantBlock), offset, &m_uniforms.push);
                M4BindBuffer(false, MetalBindings::kPushConstants, ring, offset);
                M4BindBuffer(true, MetalBindings::kPushConstants, ring, offset);
            } else {
                [enc setVertexBytes:&m_uniforms.push length:sizeof(PushConstantBlock) atIndex:MetalBindings::kPushConstants];
                [enc setFragmentBytes:&m_uniforms.push length:sizeof(PushConstantBlock) atIndex:MetalBindings::kPushConstants];
            }
            m_cache.push = m_uniforms.push;
            m_cache.pushValid = true;
        }
        auto bindBoth = [&](uint32_t index, id<MTLBuffer> buffer, NSUInteger offset) {
            BindVertexStream(index, buffer, offset);
            if (m_metal4) {
                if (m_cache.fragmentBuffers[index] != buffer || m_cache.fragmentOffsets[index] != offset) {
                    M4BindBuffer(true, index, buffer, offset);
                    m_cache.fragmentBuffers[index] = buffer;
                    m_cache.fragmentOffsets[index] = offset;
                }
                return;
            }
            if (m_cache.fragmentBuffers[index] == buffer) {
                if (m_cache.fragmentOffsets[index] != offset) {
                    [enc setFragmentBufferOffset:offset atIndex:index];
                    m_cache.fragmentOffsets[index] = offset;
                }
                return;
            }
            [enc setFragmentBuffer:buffer offset:offset atIndex:index];
            m_cache.fragmentBuffers[index] = buffer;
            m_cache.fragmentOffsets[index] = offset;
        };
        const int layoutType = m_boundShaderInfo->layoutType;
        if (layoutType == 1 || layoutType == 3) {
            // The Common / Bones blocks: a fresh ring window only for data
            // that changed since the previous draw (the matrices count only
            // for a shader that reads them) — see VKBackend's ring notes.
            const bool readsMatrices = !m_boundShaderInfo->ignoresCommonMatrices;
            if (m_uniforms.commonDirty || (m_uniforms.commonMatricesDirty && readsMatrices) || !m_haveCommon) {
                m_commonBuffer = RingAllocate(sizeof(CommonUBO), m_commonOffset, &m_uniforms.common);
                m_haveCommon = true;
                m_uniforms.commonDirty = false;
                m_uniforms.commonMatricesDirty = false;   // the window holds the current matrices too
            }
            if (m_uniforms.bonesDirty || !m_haveBones) {
                m_bonesBuffer = RingAllocate(sizeof(BonesUBO), m_bonesOffset, &m_uniforms.bones);
                m_haveBones = true;
                m_uniforms.bonesDirty = false;
            }
            bindBoth(MetalBindings::kCommon, m_commonBuffer, m_commonOffset);
            bindBoth(MetalBindings::kBones, m_bonesBuffer, m_bonesOffset);
            // The user uniform block: the terrain mega buffer's section-origin
            // table for the slab being drawn.
            if (m_boundUniformBuffer != INVALID_BUFFER) {
                auto it = m_buffers.find(m_boundUniformBuffer);
                if (it != m_buffers.end()) bindBoth(MetalBindings::kUserUniform, it->second.buffer, m_boundUniformOffset);
            }
        }
        if (layoutType == kLayoutPack) {
            // The program's uniform block: a ring window per change (and per
            // frame — a window is the frame slot's), bound to both stages;
            // the terrain family's section-origin table too.
            ShaderInfo& info = *const_cast<ShaderInfo*>(m_boundShaderInfo);
            if (info.packBlockSize > 0) {
                if (info.packDirty || info.packRingFrame != m_frameNumber || !info.packRing) {
                    info.packRing = RingAllocate(info.packBlock.size(), info.packRingOffset, info.packBlock.data());
                    info.packRingFrame = m_frameNumber;
                    info.packDirty = false;
                }
                bindBoth(MetalBindings::kPackUniforms, info.packRing, info.packRingOffset);
            }
            if (m_boundUniformBuffer != INVALID_BUFFER) {
                auto it = m_buffers.find(m_boundUniformBuffer);
                if (it != m_buffers.end()) bindBoth(MetalBindings::kUserUniform, it->second.buffer, m_boundUniformOffset);
            }
        }
        if (layoutType == 3 || layoutType == 4) {
            if (m_metal4) {
                size_t offset = 0;
                id<MTLBuffer> ring = RingAllocate(sizeof(glm::vec4), offset, &m_oitProjParams);
                M4BindBuffer(false, MetalBindings::kOitParams, ring, offset);
                M4BindBuffer(true, MetalBindings::kOitParams, ring, offset);
            } else {
                [enc setVertexBytes:&m_oitProjParams length:sizeof(glm::vec4) atIndex:MetalBindings::kOitParams];
                [enc setFragmentBytes:&m_oitProjParams length:sizeof(glm::vec4) atIndex:MetalBindings::kOitParams];
            }
            m_cache.vertexBuffers[MetalBindings::kOitParams] = nil;
            m_cache.fragmentBuffers[MetalBindings::kOitParams] = nil;
        }
    }

    void MetalBackend::BindTextures() {
        id enc = m_encoder;
        auto bind = [&](uint32_t index, id<MTLTexture> texture, id<MTLSamplerState> sampler) {
            // Both stages: the terrain vertex shader reads the lightmap.
            if (m_metal4) {
                if (m_cache.fragmentTextures[index] != texture) {
                    M4BindTexture(true, index, texture);
                    M4BindTexture(false, index, texture);
                    m_cache.fragmentTextures[index] = texture;
                    m_cache.vertexTextures[index] = texture;
                }
                if (sampler && m_cache.fragmentSamplers[index] != sampler) {
                    M4BindSampler(true, index, sampler);
                    M4BindSampler(false, index, sampler);
                    m_cache.fragmentSamplers[index] = sampler;
                    m_cache.vertexSamplers[index] = sampler;
                }
                return;
            }
            if (m_cache.fragmentTextures[index] != texture) {
                [enc setFragmentTexture:texture atIndex:index];
                [enc setVertexTexture:texture atIndex:index];
                m_cache.fragmentTextures[index] = texture;
                m_cache.vertexTextures[index] = texture;
            }
            if (sampler && m_cache.fragmentSamplers[index] != sampler) {
                [enc setFragmentSamplerState:sampler atIndex:index];
                [enc setVertexSamplerState:sampler atIndex:index];
                m_cache.fragmentSamplers[index] = sampler;
                m_cache.vertexSamplers[index] = sampler;
            }
        };
        const int layoutType = m_boundShaderInfo->layoutType;
        if (layoutType == kLayoutPack) {
            // A pack program: every slot at its own index, with its sampler.
            const uint32_t plain = m_boundShaderInfo->packSampler2D;
            for (uint32_t slot = 0; slot < MetalBindings::kPackTextureSlots; ++slot) {
                const TextureInfo* tex = m_boundTextureInfo[slot];
                if (tex && (!tex->texture || tex->bufferTexture || tex->layers > 1) && (plain & (1u << slot))) {
                    tex = m_lastTexture2D[slot];   // the slot's plain texture, not the engine's array
                }
                if (!tex || !tex->texture || tex->bufferTexture) continue;
                bind(slot, FrameTexture(*tex), tex->sampler);
            }
            return;
        }
        const TextureInfo* slot0 = nullptr;
        for (uint32_t slot = 0; slot < MetalBindings::kTextureSlots; ++slot) {
            const TextureInfo* tex = m_boundTextureInfo[slot];
            if (tex && !tex->texture) tex = nullptr;
            if (slot == 0) slot0 = tex;
            // Slot 1 falls back to slot 0's texture (the portal layout's
            // set 2 is always bound on Vulkan); slot 2 takes only a buffer
            // texture (the face map), slot 3 only a 2D one (the lightmap).
            if (slot == 1 && !tex) tex = slot0;
            if (!tex) continue;
            if (slot == 2 && !tex->bufferTexture) continue;
            if (slot != 2 && tex->bufferTexture) continue;
            // Slots 4 and 5 are the sprite arrays alone (a 2D texture there
            // would type-mismatch the shader's texture2d_array).
            if (slot >= 4 && tex->layers <= 1) continue;
            bind(MetalBindings::TextureIndexOfSlot(slot), FrameTexture(*tex), tex->bufferTexture ? nil : tex->sampler);
        }
        if ((layoutType == 3 || layoutType == 4) && m_oitCreated) {
            // Improved Transparency's set 6 for the stage last set: the depth
            // bounds stage reads the ORIGINAL bounds, the others the culled
            // ones; only the accumulation reads the coefficients.
            const OitSlot& o = m_oitSlots[m_currentFrame];
            auto texOf = [&](TextureHandle h) -> const TextureInfo* {
                auto it = m_textures.find(h);
                return it != m_textures.end() ? &it->second : nullptr;
            };
            const TextureInfo* dummy = texOf(m_oitDummy);
            const bool acc = m_oitSamplerStage == OitStage::Accumulate;
            const TextureInfo* images[3] = {
                texOf(m_oitSamplerStage == OitStage::DepthBounds || m_oitSamplerStage == OitStage::None
                          ? o.depthBoundsTex : o.culledTex),
                acc ? texOf(o.coeff0Tex) : dummy,
                acc ? texOf(o.coeff1Tex) : dummy};
            for (uint32_t i = 0; i < MetalBindings::kOitTextures; ++i) {
                if (images[i]) bind(MetalBindings::kOitTexture0 + i, images[i]->texture, images[i]->sampler);
            }
        }
    }

    bool MetalBackend::PrepareDraw() {
        if (!m_frameActive || !m_boundShaderInfo || m_oitSkipDraw) return false;
        // A draw with no texture in slot 0 is skipped, as Vulkan's is (its
        // set 0 would be whatever the previous draw bound).
        const TextureInfo* tex0 = m_boundTextureInfo[0];
        if (!tex0 || !tex0->texture) return false;
        if (!EnsureEncoder()) return false;
        id enc = m_encoder;

        if (m_drawStateDirty || m_drawConfig != m_pass.config) {
            m_drawPipeline = PipelineFor(m_state, m_boundShader, m_pass.config);
            if (ConfigHasDepth(m_pass.config)) {
                m_drawDepthStencil = DepthStencilFor(m_state);
            } else {
                // No depth/stencil attachment: Metal requires Always / no
                // write / no stencil here whatever the caller's state says.
                PipelineState none;
                none.depthTestEnabled = false;
                none.depthWriteEnabled = false;
                none.stencilTestEnabled = false;
                m_drawDepthStencil = DepthStencilFor(none);
            }
            m_drawConfig = m_pass.config;
            m_drawStateDirty = false;
        }
        id<MTLRenderPipelineState> pipeline = m_drawPipeline;
        if (!pipeline) return false;
        if (pipeline != m_cache.pipeline) {
            [enc setRenderPipelineState:pipeline];
            m_cache.pipeline = pipeline;
        }
        id<MTLDepthStencilState> ds = m_drawDepthStencil;
        if (ds != m_cache.depthStencil) {
            [enc setDepthStencilState:ds];
            m_cache.depthStencil = ds;
        }
        if (m_state.stencilTestEnabled && m_cache.stencilRef != m_state.stencilReference) {
            [enc setStencilReferenceValue:m_state.stencilReference];
            m_cache.stencilRef = m_state.stencilReference;
        }
        const int cull = static_cast<int>(m_state.cullMode);
        if (cull != m_cache.cull) {
            [enc setCullMode:m_state.cullMode == CullMode::None    ? MTLCullModeNone
                             : m_state.cullMode == CullMode::Front ? MTLCullModeFront
                                                                   : MTLCullModeBack];
            m_cache.cull = cull;
        }
        // Counter-clockwise = front, as on OpenGL and on Vulkan's flipped
        // viewport: the image is upright on all three.
        // A pack texture target's flipped viewport (ApplyViewport) mirrors
        // every triangle's screen winding: the rule flips with it.
        const bool ccwFront = (m_state.frontFace == FrontFace::CounterClockwise) != ConfigFromPass(m_pass.config);
        const int winding = ccwFront ? 1 : 0;
        if (winding != m_cache.winding) {
            [enc setFrontFacingWinding:ccwFront ? MTLWindingCounterClockwise : MTLWindingClockwise];
            m_cache.winding = winding;
        }
        const int fill = static_cast<int>(m_state.polygonMode);
        if (fill != m_cache.fill) {
            [enc setTriangleFillMode:m_state.polygonMode == PolygonMode::Line ? MTLTriangleFillModeLines
                                                                              : MTLTriangleFillModeFill];
            m_cache.fill = fill;
        }
        const float biasC = m_state.depthBiasEnabled ? m_state.depthBiasConstant : 0.0f;
        const float biasS = m_state.depthBiasEnabled ? m_state.depthBiasSlope : 0.0f;
        if (!m_cache.depthBiasValid || biasC != m_cache.biasConstant || biasS != m_cache.biasSlope) {
            [enc setDepthBias:biasC slopeScale:biasS clamp:0.0f];
            m_cache.depthBiasValid = true;
            m_cache.biasConstant = biasC;
            m_cache.biasSlope = biasS;
        }
        const int clip = m_state.depthClampEnabled ? 1 : 0;
        if (clip != m_cache.clip) {
            [enc setDepthClipMode:clip ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
            m_cache.clip = clip;
        }
        BindUniforms();
        BindTextures();
        return true;
    }

    void MetalBackend::BindVertexBuffer(BufferHandle vbo, uint32_t /*stride*/) { m_megaVBO = vbo; }
    void MetalBackend::BindIndexBuffer(BufferHandle ibo) { m_megaIBO = ibo; }

    void MetalBackend::DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex,
                                             IndexType indexType) {
        ++m_counters.draws;
        if (m_boundShader == INVALID_SHADER || indexCount == 0) return;
        auto vb = m_buffers.find(m_megaVBO);
        auto ib = m_buffers.find(m_megaIBO);
        if (vb == m_buffers.end() || ib == m_buffers.end()) return;
        if (!PrepareDraw()) return;
        BindVertexStream(MetalBindings::kVertexStream, vb->second.buffer, 0);
        const MTLIndexType type = indexType == IndexType::Uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        if (m_metal4) {
            M4DrawIndexed(ToPrimitive(m_state.primitiveType), indexCount, type, ib->second.buffer, indexByteOffset, 1, baseVertex);
            return;
        }
        [m_encoder drawIndexedPrimitives:ToPrimitive(m_state.primitiveType)
                              indexCount:indexCount
                               indexType:type
                             indexBuffer:ib->second.buffer
                       indexBufferOffset:indexByteOffset
                           instanceCount:1
                              baseVertex:baseVertex
                            baseInstance:0];
    }

    void MetalBackend::MultiDrawIndexedBaseVertex(const int32_t* indexCounts, const size_t* indexByteOffsets,
                                                  const int32_t* baseVertices, uint32_t drawCount,
                                                  IndexType indexType) {
        m_counters.draws += drawCount;
        if (m_boundShader == INVALID_SHADER || drawCount == 0) return;
        auto vb = m_buffers.find(m_megaVBO);
        auto ib = m_buffers.find(m_megaIBO);
        if (vb == m_buffers.end() || ib == m_buffers.end()) return;
        if (!PrepareDraw()) return;
        // State once for the batch; each section is one draw (a few hundred
        // nanoseconds of encoding each).
        BindVertexStream(MetalBindings::kVertexStream, vb->second.buffer, 0);
        const MTLPrimitiveType primitive = ToPrimitive(m_state.primitiveType);
        const MTLIndexType type = indexType == IndexType::Uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        id<MTLBuffer> indices = ib->second.buffer;
        if (m_metal4) {
            for (uint32_t i = 0; i < drawCount; ++i) {
                if (indexCounts[i] <= 0) continue;
                M4DrawIndexed(primitive, static_cast<uint32_t>(indexCounts[i]), type, indices, indexByteOffsets[i], 1, baseVertices[i]);
            }
            return;
        }
        id enc = m_encoder;
        for (uint32_t i = 0; i < drawCount; ++i) {
            if (indexCounts[i] <= 0) continue;
            [enc drawIndexedPrimitives:primitive
                            indexCount:static_cast<NSUInteger>(indexCounts[i])
                             indexType:type
                           indexBuffer:indices
                     indexBufferOffset:indexByteOffsets[i]
                         instanceCount:1
                            baseVertex:baseVertices[i]
                          baseInstance:0];
        }
    }

    void MetalBackend::DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset) {
        ++m_counters.draws;
        if (mesh == INVALID_MESH || m_boundShader == INVALID_SHADER || indexCount == 0) return;
        auto meshIt = m_meshes.find(mesh);
        if (meshIt == m_meshes.end()) return;
        auto vb = m_buffers.find(meshIt->second.vertexBuffer);
        auto ib = m_buffers.find(meshIt->second.indexBuffer);
        if (vb == m_buffers.end() || ib == m_buffers.end()) return;
        if (!PrepareDraw()) return;
        BindVertexStream(MetalBindings::kVertexStream, vb->second.buffer, 0);
        if (m_metal4) {
            M4DrawIndexed(ToPrimitive(m_state.primitiveType), indexCount, MTLIndexTypeUInt32, ib->second.buffer,
                          static_cast<size_t>(indexOffset) * sizeof(uint32_t), 1, 0);
            return;
        }
        [m_encoder drawIndexedPrimitives:ToPrimitive(m_state.primitiveType)
                              indexCount:indexCount
                               indexType:MTLIndexTypeUInt32
                             indexBuffer:ib->second.buffer
                       indexBufferOffset:static_cast<NSUInteger>(indexOffset) * sizeof(uint32_t)];
    }

    void MetalBackend::DrawIndexedInstanced(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset,
                                            uint32_t instanceCount, uint32_t instanceByteOffset) {
        ++m_counters.draws;
        if (mesh == INVALID_MESH || m_boundShader == INVALID_SHADER || indexCount == 0 || instanceCount == 0) return;
        auto meshIt = m_meshes.find(mesh);
        if (meshIt == m_meshes.end()) return;
        auto vb = m_buffers.find(meshIt->second.vertexBuffer);
        auto ib = m_buffers.find(meshIt->second.indexBuffer);
        auto inst = m_buffers.find(meshIt->second.instanceBuffer);
        if (vb == m_buffers.end() || ib == m_buffers.end() || inst == m_buffers.end()) return;
        if (!PrepareDraw()) return;
        BindVertexStream(MetalBindings::kVertexStream, vb->second.buffer, 0);
        BindVertexStream(MetalBindings::kInstanceStream, inst->second.buffer, instanceByteOffset);
        if (m_metal4) {
            M4DrawIndexed(ToPrimitive(m_state.primitiveType), indexCount, MTLIndexTypeUInt32, ib->second.buffer,
                          static_cast<size_t>(indexOffset) * sizeof(uint32_t), instanceCount, 0);
            return;
        }
        [m_encoder drawIndexedPrimitives:ToPrimitive(m_state.primitiveType)
                              indexCount:indexCount
                               indexType:MTLIndexTypeUInt32
                             indexBuffer:ib->second.buffer
                       indexBufferOffset:static_cast<NSUInteger>(indexOffset) * sizeof(uint32_t)
                           instanceCount:instanceCount];
    }

    void MetalBackend::DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) {
        ++m_counters.draws;
        if (mesh == INVALID_MESH || m_boundShader == INVALID_SHADER || vertexCount == 0) return;
        auto meshIt = m_meshes.find(mesh);
        if (meshIt == m_meshes.end()) return;
        auto vb = m_buffers.find(meshIt->second.vertexBuffer);
        if (vb == m_buffers.end()) return;
        if (!PrepareDraw()) return;
        BindVertexStream(MetalBindings::kVertexStream, vb->second.buffer, 0);
        [m_encoder drawPrimitives:ToPrimitive(m_state.primitiveType) vertexStart:firstVertex vertexCount:vertexCount];
    }

    // ========================================================================
    // IN-PASS CLEARS
    // ========================================================================

    void MetalBackend::DrawClearQuad(bool color, bool depth, bool stencil, float depthValue, const MTLScissorRect& rect) {
        if (!m_encoder || !m_internalLibrary) return;
        if (!ConfigHasDepth(m_pass.config)) {
            depth = stencil = false;   // nothing to clear there
            if (!color) return;
        }
        // A texture target (a shader pack's): every colour attachment clears,
        // as glClear did; the engine's own passes clear attachment 0 only,
        // as vkCmdClearAttachments did.
        const bool fromPass = ConfigFromPass(m_pass.config);
        AttachmentConfig att = AttachmentsFor(m_pass.config);
        if (fromPass) {
            att.count = static_cast<uint32_t>(std::min<size_t>(m_pass.formats.size(), 8));
            for (uint32_t i = 0; i < att.count; ++i) att.formats[i] = m_pass.formats[i];
        }
        const uint32_t colorOutputs = fromPass && color ? att.count : 1u;
        uint64_t key = static_cast<uint64_t>(m_pass.config) | (color ? 8u : 0u);
        if (fromPass) key |= static_cast<uint64_t>(m_pass.attachmentsKey) << 8;
        id<MTLRenderPipelineState> pipeline = nil;
        auto it = m_clearPipelines.find(key);
        if (it != m_clearPipelines.end()) {
            pipeline = it->second;
        } else {
            @autoreleasepool {
                MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
                desc.vertexFunction = [m_internalLibrary newFunctionWithName:@"clear_vertex"];
                NSString* fragment = colorOutputs > 1 ? [NSString stringWithFormat:@"clear_fragment%u", colorOutputs] : @"clear_fragment";
                desc.fragmentFunction = [m_internalLibrary newFunctionWithName:fragment];
                for (uint32_t i = 0; i < att.count; ++i) {
                    desc.colorAttachments[i].pixelFormat = att.formats[i];
                    desc.colorAttachments[i].writeMask = (color && i < colorOutputs) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
                }
                desc.depthAttachmentPixelFormat = ConfigHasDepth(m_pass.config) ? kDepthFormat : MTLPixelFormatInvalid;
                desc.stencilAttachmentPixelFormat = ConfigHasDepth(m_pass.config) ? kDepthFormat : MTLPixelFormatInvalid;
                desc.label = [NSString stringWithFormat:@"Clear quad (config %u%s)", static_cast<unsigned>(m_pass.config),
                                                        color ? ", colour" : ""];
                NSError* error = nil;
                pipeline = [m_device newRenderPipelineStateWithDescriptor:desc error:&error];
                if (!pipeline) {
                    Log::Error("MetalBackend: clear pipeline failed: %s",
                               error ? error.localizedDescription.UTF8String : "unknown");
                }
                m_clearPipelines[key] = pipeline;
            }
        }
        if (!pipeline) return;
        PipelineState s;
        s.depthTestEnabled = depth;            // Always below: a test that writes
        s.depthCompareOp = CompareOp::Always;
        s.depthWriteEnabled = depth;
        s.stencilTestEnabled = stencil;
        s.stencilCompareOp = CompareOp::Always;
        s.stencilPassOp = StencilOp::Replace;
        s.stencilFailOp = StencilOp::Replace;
        s.stencilDepthFailOp = StencilOp::Replace;
        s.stencilWriteMask = 0xFFu;
        id enc = m_encoder;
        struct { float color[4]; float depth; float pad[3]; } params = {
            {m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]}, depthValue, {0, 0, 0}};
        [enc setRenderPipelineState:pipeline];
        [enc setDepthStencilState:DepthStencilFor(s)];
        [enc setStencilReferenceValue:0];
        [enc setCullMode:MTLCullModeNone];
        [enc setTriangleFillMode:MTLTriangleFillModeFill];
        [enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
        [enc setDepthClipMode:MTLDepthClipModeClip];
        [enc setViewport:MTLViewport{0, 0, static_cast<double>(ActiveWidth()), static_cast<double>(ActiveHeight()), 0, 1}];
        [enc setScissorRect:rect];
        if (m_metal4) {
            size_t offset = 0;
            id<MTLBuffer> ring = RingAllocate(sizeof(params), offset, &params);
            M4BindBuffer(false, 0, ring, offset);
            M4BindBuffer(true, 0, ring, offset);
        } else {
            [enc setVertexBytes:&params length:sizeof(params) atIndex:0];
            [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        }
        ++m_counters.draws;
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        // What the clear touched; textures and the other buffers stay bound.
        m_cache.pipeline = nil;
        m_cache.depthStencil = nil;
        m_cache.stencilRef = 0;
        m_cache.cull = static_cast<int>(CullMode::None);
        m_cache.fill = static_cast<int>(PolygonMode::Fill);
        m_cache.clip = 0;
        m_cache.depthBiasValid = true;
        m_cache.biasConstant = m_cache.biasSlope = 0.0f;
        m_cache.pushValid = false;   // buffer 0 holds the clear's parameters
        m_cache.vertexBuffers[0] = nil;
        m_cache.fragmentBuffers[0] = nil;
        m_cache.viewportValid = false;
        m_cache.scissorValid = false;
        ApplyViewport();
        ApplyScissor();
    }

    // ========================================================================
    // IMPROVED TRANSPARENCY — MC 26.3 WAVELET OIT
    // ========================================================================

    uint8_t MetalBackend::OitPassConfig(OitPass pass) {
        switch (pass) {
            case OitPass::Transmittance: case OitPass::CloudTransmittance: return 2;
            case OitPass::Accumulate:    case OitPass::CloudAccumulate:    return 3;
            default:                                                       return 1;
        }
    }

    ShaderHandle MetalBackend::OitVariantFor(ShaderHandle engine) {
        const size_t stage = static_cast<size_t>(m_oitStage);
        OitVariantSet& v = m_oitVariants[engine];
        if (v.tried[stage]) return v.shader[stage];
        v.tried[stage] = true;
        auto it = m_shaders.find(engine);
        if (it == m_shaders.end() || it->second.fragPath.empty()) return INVALID_SHADER;
        const ShaderInfo base = it->second;   // copy: m_shaders may rehash below
        if (base.layoutType != 0 && base.layoutType != 1) return INVALID_SHADER;
        // shaders/<name>.frag -> shaders/<name>_oit_<stage>.frag, which
        // LoadFunction maps to shaders/metal/<name>_oit_<stage>_vk.frag.metal.
        const size_t dot = base.fragPath.rfind(".frag");
        if (dot == std::string::npos) return INVALID_SHADER;
        const char* suffix = m_oitStage == OitStage::DepthBounds   ? "_oit_db"
                           : m_oitStage == OitStage::Transmittance ? "_oit_tr" : "_oit_ac";
        const std::string fragPath = base.fragPath.substr(0, dot) + suffix + ".frag";
        const std::string stem = base.fragPath.substr(0, dot);
        const size_t slash = stem.find_last_of('/');
        const std::string metalPath = (slash == std::string::npos ? std::string() : stem.substr(0, slash + 1)) +
                                      "metal/" + stem.substr(slash == std::string::npos ? 0 : slash + 1) + suffix +
                                      "_vk.frag.metal";
        if (!std::filesystem::exists(metalPath)) return INVALID_SHADER;   // not a participating shader
        const ShaderHandle variant = CreateShaderFromFiles(base.vertPath, fragPath);
        if (variant == INVALID_SHADER) return INVALID_SHADER;
        ShaderInfo& info = m_shaders[variant];
        info.layoutType = base.layoutType == 1 ? 3 : 4;
        info.vertexLayout = base.vertexLayout;
        info.instanceLayout = base.instanceLayout;
        info.ignoresCommonMatrices = base.ignoresCommonMatrices;
        v.shader[stage] = variant;
        return variant;
    }

    void MetalBackend::SetOitStage(OitStage stage, const glm::vec4& projParams, bool depthBoundsWriteDepth) {
        m_oitStage = stage;
        m_oitProjParams = projParams;
        m_oitDbWritesDepth = depthBoundsWriteDepth;
        m_oitSkipDraw = false;
        // The next BindShader resolves the stage's variant; nothing draws
        // with the one bound now.
        m_boundShader = INVALID_SHADER;
        m_boundShaderInfo = nullptr;
        if (stage != OitStage::None) m_oitSamplerStage = stage;
        SetPipelineState(m_requestedState);   // re-splice for the new stage
    }

    bool MetalBackend::OitEnsureTargets(int width, int height) {
        if (width <= 0 || height <= 0 || !m_frameDepthPreserved) return false;
        // Built on the other frame depth (the scaled scene's vs the window's):
        // the passes would attach the wrong image — rebuild.
        if (m_oitCreated && m_oitWidth == width && m_oitHeight == height && m_oitOnScene == m_sceneActive) return true;
        OitDestroyTargets();
        @autoreleasepool {
            auto make = [&](MTLPixelFormat format, MTLTextureUsage usage) {
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                              width:static_cast<NSUInteger>(width)
                                                                                             height:static_cast<NSUInteger>(height)
                                                                                          mipmapped:NO];
                d.usage = usage;
                d.storageMode = MTLStorageModePrivate;
                id<MTLTexture> t = [m_device newTextureWithDescriptor:d];
                M4Resident(t);
                return t;
            };
            const MTLTextureUsage colorUsage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            for (uint32_t s = 0; s < kFramesInFlight; ++s) {
                OitSlot& o = m_oitSlots[s];
                o.depthBounds = make(MTLPixelFormatRGBA32Float, colorUsage);
                o.culled = make(MTLPixelFormatRGBA32Float, colorUsage);
                o.coeff0 = make(MTLPixelFormatRGBA16Float, colorUsage);
                o.coeff1 = make(MTLPixelFormatRGBA16Float, colorUsage);
                o.accumulate = make(MTLPixelFormatRGBA16Float, colorUsage);
                o.cloudDepth = make(kDepthFormat, MTLTextureUsageRenderTarget);
                id<MTLTexture> frameDepth = FrameDepthTex(s);
                if (!o.depthBounds || !o.culled || !o.coeff0 || !o.coeff1 || !o.accumulate || !o.cloudDepth || !frameDepth) {
                    Log::Warning("MetalBackend: Improved Transparency targets could not be created");
                    m_oitCreated = true;
                    OitDestroyTargets();
                    return false;
                }
                o.depthBounds.label = [NSString stringWithFormat:@"OIT depth bounds [%u]", s];
                o.culled.label = [NSString stringWithFormat:@"OIT culled [%u]", s];
                o.coeff0.label = [NSString stringWithFormat:@"OIT coeff0 [%u]", s];
                o.coeff1.label = [NSString stringWithFormat:@"OIT coeff1 [%u]", s];
                o.accumulate.label = [NSString stringWithFormat:@"OIT accumulate [%u]", s];
                o.cloudDepth.label = [NSString stringWithFormat:@"OIT cloud depth [%u]", s];
                o.depthBoundsTex = WrapTexture(o.depthBounds, MTLSamplerMinMagFilterNearest);
                o.culledTex = WrapTexture(o.culled, MTLSamplerMinMagFilterNearest);
                o.coeff0Tex = WrapTexture(o.coeff0, MTLSamplerMinMagFilterNearest);
                o.coeff1Tex = WrapTexture(o.coeff1, MTLSamplerMinMagFilterNearest);
                o.accumulateTex = WrapTexture(o.accumulate, MTLSamplerMinMagFilterNearest);
                o.frameDepthTex = WrapTexture(frameDepth, MTLSamplerMinMagFilterNearest);
            }
        }
        if (m_oitDummy == INVALID_TEXTURE) {
            const unsigned char none[4] = {0, 0, 0, 0};
            m_oitDummy = CreateTexture2D(1, 1, TextureFormat::RGBA8, none);
        }
        m_oitCreated = true;
        m_oitOnScene = m_sceneActive;
        m_oitWidth = width;
        m_oitHeight = height;
        Log::Info("MetalBackend: Improved Transparency targets %dx%d (%d frame slots)", width, height, kFramesInFlight);
        return true;
    }

    void MetalBackend::OitDestroyTargets() {
        if (!m_oitCreated) return;
        if (m_oitPassOpen) OitEndPass();
        for (OitSlot& o : m_oitSlots) {
            for (TextureHandle h : {o.depthBoundsTex, o.culledTex, o.coeff0Tex, o.coeff1Tex, o.accumulateTex, o.frameDepthTex}) {
                if (h != INVALID_TEXTURE) EraseTextureEntry(h);
            }
            for (id<MTLTexture> t : {o.depthBounds, o.culled, o.coeff0, o.coeff1, o.accumulate, o.cloudDepth}) M4Release(t);
            o = OitSlot{};
        }
        // The pipelines built for the OIT passes, and the variants: rebuilt
        // when the option comes back.
        for (auto it = m_pipelines.begin(); it != m_pipelines.end();) {
            if (ConfigIsOit(it->second.config)) it = m_pipelines.erase(it);
            else ++it;
        }
        m_drawStateDirty = true;
        std::vector<ShaderHandle> variants;
        for (auto& [engine, v] : m_oitVariants) {
            for (ShaderHandle s : v.shader) if (s != INVALID_SHADER) variants.push_back(s);
        }
        m_oitVariants.clear();
        for (ShaderHandle s : variants) DestroyShader(s);
        m_oitCreated = false;
        m_oitWidth = m_oitHeight = 0;
    }

    bool MetalBackend::OitBeginPass(OitPass pass, bool clearColor) {
        if (!m_frameActive || !m_oitCreated || m_oitPassOpen || !m_frameDepthPreserved) return false;
        if (m_activeTarget != INVALID_RENDER_TARGET) return false;
        if (static_cast<uint32_t>(m_oitWidth) != FrameWidth() || static_cast<uint32_t>(m_oitHeight) != FrameHeight() ||
            m_oitOnScene != m_sceneActive) {
            return false;
        }
        // The frame's pass ends with its depth stored (preserved), so the OIT
        // pass can test against it — or, for the clouds, sample it. Its
        // encoder is opened only the first time (the clears have to land);
        // a resumed frame pass that drew nothing between two OIT passes is
        // dropped unopened — it used to run as an empty encoder, a load and
        // store of the full-res colour + D32S8 (~60 MB) between every pair
        // of OIT passes (Xcode capture, 2026-10-06).
        if (m_pass.kind != PassKind::Frame) return false;
        if (!m_frameOpened && !EnsureEncoder()) return false;
        if (!m_encoder) m_pass.clears = false;   // nothing drawn, nothing to clear: no encoder
        EndPass();

        const OitSlot& o = m_oitSlots[m_currentFrame];
        const uint8_t config = OitPassConfig(pass);
        const bool cloud = pass == OitPass::CloudDepthBounds || pass == OitPass::CloudTransmittance ||
                           pass == OitPass::CloudAccumulate;
        id<MTLTexture> colors[2] = {nil, nil};
        switch (pass) {
            case OitPass::DepthBounds:        colors[0] = o.depthBounds; break;
            case OitPass::DepthBoundsCull:
            case OitPass::CloudDepthBounds:   colors[0] = o.culled; break;
            case OitPass::Transmittance:
            case OitPass::CloudTransmittance: colors[0] = o.coeff0; colors[1] = o.coeff1; break;
            case OitPass::Accumulate:
            case OitPass::CloudAccumulate:    colors[0] = o.accumulate; break;
        }
        MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
        const uint32_t count = config == 2 ? 2u : 1u;
        for (uint32_t i = 0; i < count; ++i) {
            d.colorAttachments[i].texture = colors[i];
            d.colorAttachments[i].loadAction = clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
            d.colorAttachments[i].storeAction = MTLStoreActionStore;
            // MC's clear values: the depth bounds (-FLT_MAX, 0, 0, 0) so MAX
            // finds the nearest and farthest; the rest zero.
            d.colorAttachments[i].clearColor = config == 1 ? MTLClearColorMake(-3.4028235e38, 0, 0, 0)
                                                           : MTLClearColorMake(0, 0, 0, 0);
        }
        id<MTLTexture> depth = cloud ? o.cloudDepth : FrameDepthTex(m_currentFrame);
        d.depthAttachment.texture = depth;
        d.depthAttachment.loadAction = MTLLoadActionLoad;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.stencilAttachment.texture = depth;
        d.stencilAttachment.loadAction = MTLLoadActionLoad;
        d.stencilAttachment.storeAction = MTLStoreActionStore;
        m_pass = Pass{};
        m_pass.kind = PassKind::Oit;
        m_pass.desc = d;
        m_pass.width = static_cast<uint32_t>(m_oitWidth);
        m_pass.height = static_cast<uint32_t>(m_oitHeight);
        m_pass.config = config;
        m_pass.colorCount = static_cast<uint8_t>(count);
        m_pass.clears = clearColor;
        m_oitPassOpen = true;
        m_oitOpenPass = pass;
        SetViewport(0, 0, m_oitWidth, m_oitHeight);
        ClearScissorRect();
        return true;
    }

    void MetalBackend::OitEndPass() {
        if (!m_oitPassOpen) return;
        EndPass();
        m_oitPassOpen = false;
        // Back to the frame, colour and depth as they were.
        BeginFramePass(/*resume=*/true);
        SetViewport(0, 0, static_cast<int>(FrameWidth()), static_cast<int>(FrameHeight()));
        ClearScissorRect();
    }

    TextureHandle MetalBackend::OitTexture(OitImage image) const {
        if (!m_oitCreated) return INVALID_TEXTURE;
        const OitSlot& o = m_oitSlots[m_currentFrame];
        switch (image) {
            case OitImage::DepthBounds:       return o.depthBoundsTex;
            case OitImage::DepthBoundsCulled: return o.culledTex;
            case OitImage::Coeff0:            return o.coeff0Tex;
            case OitImage::Coeff1:            return o.coeff1Tex;
            case OitImage::Accumulate:        return o.accumulateTex;
            case OitImage::FrameDepth:        return o.frameDepthTex;
        }
        return INVALID_TEXTURE;
    }

} // namespace Render

#endif // HAS_METAL
