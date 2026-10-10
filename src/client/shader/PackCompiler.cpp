// File: src/client/shader/PackCompiler.cpp
#include "client/shader/PackCompiler.hpp"

#include "common/core/Log.hpp"

#if HAS_PACK_SPIRV
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_cross.hpp>
#include <spirv_msl.hpp>
#endif

#include <algorithm>
#include <cstring>
#include <set>
#include <mutex>

namespace Shaders::PackCompiler {

#if HAS_PACK_SPIRV

    namespace {
        // glslang's process state, once per process (thread-safe init guard;
        // the compiler itself is used from the render thread only).
        void EnsureGlslang() {
            static std::once_flag s_once;
            std::call_once(s_once, [] { glslang::InitializeProcess(); });
        }

        bool CompileStage(EShLanguage lang, const std::string& glsl, const std::string& label,
                          std::vector<uint32_t>& spirv, std::string& error) {
            glslang::TShader shader(lang);
            const char* src = glsl.c_str();
            shader.setStrings(&src, 1);
            shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
            // Vulkan 1.0 / SPIR-V 1.0: what the engine's instance and device
            // are (spirv-val rejects a newer binary under 1.0 semantics).
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
            // Vulkan GLSL: loose uniforms are errors, which is the point —
            // the translation gathered them into the block.
            const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
            if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
                error = label + ": " + shader.getInfoLog();
                return false;
            }
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(messages)) {
                error = label + ": " + program.getInfoLog();
                return false;
            }
            glslang::SpvOptions options;
            options.disableOptimizer = true;   // ENABLE_OPT is off; glslang then emits straight SPIR-V
            options.validate = false;
            spv::SpvBuildLogger logger;
            spirv.clear();
            glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &options);
            if (spirv.empty()) {
                error = label + ": no SPIR-V produced " + logger.getAllMessages();
                return false;
            }
            return true;
        }

        Uniform::Kind KindOf(const spirv_cross::SPIRType& t) {
            using BT = spirv_cross::SPIRType::BaseType;
            if (t.columns > 1) {
                if (t.basetype == BT::Float && t.columns == 4 && t.vecsize == 4) return Uniform::Mat4;
                if (t.basetype == BT::Float && t.columns == 3 && t.vecsize == 3) return Uniform::Mat3;
                return Uniform::Other;
            }
            switch (t.basetype) {
                case BT::Float:
                    return t.vecsize == 1 ? Uniform::Float : t.vecsize == 2 ? Uniform::Vec2 : t.vecsize == 3 ? Uniform::Vec3 : Uniform::Vec4;
                case BT::Int:
                case BT::UInt:
                    return t.vecsize == 1 ? Uniform::Int : t.vecsize == 2 ? Uniform::IVec2 : t.vecsize == 3 ? Uniform::IVec3 : Uniform::IVec4;
                case BT::Boolean:
                    return t.vecsize == 1 ? Uniform::Bool : Uniform::Other;
                default:
                    return Uniform::Other;
            }
        }

        // The stage's PackUniforms block and samplers into the program's
        // union (a member or sampler both stages declare is one entry).
        bool Reflect(const std::vector<uint32_t>& spirv, bool vertex, const std::string& label, Program& out) {
            try {
                spirv_cross::Compiler comp(spirv);
                const spirv_cross::ShaderResources res = comp.get_shader_resources();
                for (const spirv_cross::Resource& ub : res.uniform_buffers) {
                    const std::string blockName = comp.get_name(ub.base_type_id);
                    if (blockName != kUniformBlockName && comp.get_name(ub.id) != kUniformBlockName) continue;
                    const spirv_cross::SPIRType& type = comp.get_type(ub.base_type_id);
                    out.uniformBlockSize = std::max<uint32_t>(out.uniformBlockSize,
                                                              static_cast<uint32_t>(comp.get_declared_struct_size(type)));
                    for (uint32_t i = 0; i < type.member_types.size(); ++i) {
                        Uniform u;
                        u.name = comp.get_member_name(ub.base_type_id, i);
                        const spirv_cross::SPIRType& mt = comp.get_type(type.member_types[i]);
                        u.kind = KindOf(mt);
                        u.offset = comp.type_struct_member_offset(type, i);
                        u.size = static_cast<uint32_t>(comp.get_declared_struct_member_size(type, i));
                        if (!mt.array.empty()) {
                            u.arrayLength = mt.array[0];
                            u.arrayStride = comp.type_struct_member_array_stride(type, i);
                        }
                        auto existing = std::find_if(out.uniforms.begin(), out.uniforms.end(),
                                                     [&](const Uniform& e) { return e.name == u.name; });
                        if (existing == out.uniforms.end()) out.uniforms.push_back(u);
                        else if (existing->offset != u.offset) {
                            out.error = label + ": uniform " + u.name + " sits at different offsets in the two stages";
                            return false;
                        }
                    }
                }
                if (vertex) {
                    for (const spirv_cross::Resource& in : res.stage_inputs) {
                        out.vertexInputLocations.push_back(comp.get_decoration(in.id, spv::DecorationLocation));
                    }
                }
                for (const spirv_cross::Resource& img : res.sampled_images) {
                    Sampler s;
                    s.name = comp.get_name(img.id);
                    s.binding = comp.get_decoration(img.id, spv::DecorationBinding);
                    const spirv_cross::SPIRType& imgType = comp.get_type(img.type_id);
                    s.plain2D = imgType.image.dim == spv::Dim2D && !imgType.image.arrayed && !imgType.image.ms;
                    const uint32_t set = comp.get_decoration(img.id, spv::DecorationDescriptorSet);
                    if (set != kSamplerSet || s.binding >= kMaxSamplers) {
                        out.error = label + ": sampler " + s.name + " at set " + std::to_string(set) + " binding " +
                                    std::to_string(s.binding) + " is outside the pack layout";
                        return false;
                    }
                    auto existing = std::find_if(out.samplers.begin(), out.samplers.end(),
                                                 [&](const Sampler& e) { return e.binding == s.binding; });
                    if (existing == out.samplers.end()) { (vertex ? s.vertexStage : s.fragmentStage) = true; out.samplers.push_back(s); }
                    else (vertex ? existing->vertexStage : existing->fragmentStage) = true;
                }
            } catch (const std::exception& e) {
                out.error = label + ": reflection failed: " + e.what();
                return false;
            }
            return true;
        }
    } // namespace

    namespace {
        std::string GlslTypeName(const spirv_cross::SPIRType& t) {
            using BT = spirv_cross::SPIRType::BaseType;
            std::string base;
            switch (t.basetype) {
                case BT::Float:   base = t.columns > 1 ? "mat" : t.vecsize > 1 ? "vec" : "float"; break;
                case BT::Int:     base = t.vecsize > 1 ? "ivec" : "int"; break;
                case BT::UInt:    base = t.vecsize > 1 ? "uvec" : "uint"; break;
                case BT::Boolean: base = t.vecsize > 1 ? "bvec" : "bool"; break;
                default: return "";
            }
            if (t.columns > 1) {
                base += std::to_string(t.columns);
                if (t.columns != t.vecsize) base += "x" + std::to_string(t.vecsize);
            } else if (t.vecsize > 1) {
                base += std::to_string(t.vecsize);
            }
            return base;
        }

        // A fragment input the vertex stage never declares (a varying
        // declared under a #if the vertex stage took the other way): Vulkan
        // pairs stage interfaces, so the vertex stage gets an output of
        // that type at that location, unwritten — what OpenGL read too.
        // Returns the declarations to append to the vertex GLSL.
        std::string UnwrittenInputs(const std::vector<uint32_t>& vertSpirv, const std::vector<uint32_t>& fragSpirv) {
            std::string extra;
            try {
                spirv_cross::Compiler vert(vertSpirv), frag(fragSpirv);
                std::set<uint32_t> written;
                for (const spirv_cross::Resource& o : vert.get_shader_resources().stage_outputs) {
                    const spirv_cross::SPIRType& t = vert.get_type(o.type_id);
                    const uint32_t loc = vert.get_decoration(o.id, spv::DecorationLocation);
                    uint32_t count = std::max<uint32_t>(1, t.columns);
                    for (uint32_t a : t.array) count *= std::max<uint32_t>(1, a);
                    for (uint32_t i = 0; i < count; ++i) written.insert(loc + i);
                }
                for (const spirv_cross::Resource& in : frag.get_shader_resources().stage_inputs) {
                    const spirv_cross::SPIRType& t = frag.get_type(in.type_id);
                    const uint32_t loc = frag.get_decoration(in.id, spv::DecorationLocation);
                    if (written.count(loc)) continue;
                    const std::string type = GlslTypeName(t);
                    if (type.empty()) continue;
                    std::string array;
                    for (uint32_t a : t.array) array += "[" + std::to_string(a) + "]";
                    extra += "layout(location = " + std::to_string(loc) + ") out " + type + " sp_unwritten_" + std::to_string(loc) + array + ";\n";
                }
            } catch (const std::exception&) {
                return "";
            }
            return extra;
        }
    } // namespace

    bool Available() { return true; }

    bool Preprocess(const std::string& glsl, bool fragment, std::string& out) {
        EnsureGlslang();
        glslang::TShader shader(fragment ? EShLangFragment : EShLangVertex);
        const char* src = glsl.c_str();
        shader.setStrings(&src, 1);
        glslang::TShader::ForbidIncluder includer;   // includes are resolved before this
        out.clear();
        if (shader.preprocess(GetDefaultResources(), 330, ECoreProfile, false, false, EShMsgDefault, &out, includer)) return true;
        out = shader.getInfoLog();
        return false;
    }

    bool Compile(const std::string& vertexGlsl, const std::string& fragmentGlsl,
                 const std::string& label, Program& out) {
        EnsureGlslang();
        out = Program{};
        if (!CompileStage(EShLangVertex, vertexGlsl, label + ".vsh", out.vertexSpirv, out.error)) return false;
        if (!CompileStage(EShLangFragment, fragmentGlsl, label + ".fsh", out.fragmentSpirv, out.error)) return false;
        const std::string unwritten = UnwrittenInputs(out.vertexSpirv, out.fragmentSpirv);
        if (!unwritten.empty()) {
            Log::Info("[PackCompiler] %s: vertex outputs added for fragment inputs the vertex stage never declared:\n%s",
                      label.c_str(), unwritten.c_str());
            if (!CompileStage(EShLangVertex, vertexGlsl + "\n" + unwritten, label + ".vsh", out.vertexSpirv, out.error)) return false;
        }
        if (!Reflect(out.vertexSpirv, true, label, out)) return false;
        if (!Reflect(out.fragmentSpirv, false, label, out)) return false;
        return true;
    }

    bool ToMetal(const std::vector<uint32_t>& spirv, bool vertex, uint32_t uniformBufferIndex,
                 uint32_t userUniformIndex, std::string& msl, std::string& error) {
        try {
            spirv_cross::CompilerMSL comp(spirv);
            spirv_cross::CompilerMSL::Options opts;
            opts.platform = spirv_cross::CompilerMSL::Options::macOS;
            opts.set_msl_version(2, 3);
            opts.enable_decoration_binding = false;
            comp.set_msl_options(opts);
            const spv::ExecutionModel model = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
            // The uniform blocks and the samplers at the engine's indices.
            spirv_cross::MSLResourceBinding ub;
            ub.stage = model;
            ub.desc_set = kUniformSet;
            ub.binding = 0;
            ub.msl_buffer = uniformBufferIndex;
            comp.add_msl_resource_binding(ub);
            spirv_cross::MSLResourceBinding user;
            user.stage = model;
            user.desc_set = kUserUniformSet;
            user.binding = 0;
            user.msl_buffer = userUniformIndex;
            comp.add_msl_resource_binding(user);
            for (uint32_t slot = 0; slot < kMaxSamplers; ++slot) {
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                b.desc_set = kSamplerSet;
                b.binding = slot;
                b.msl_texture = slot;
                b.msl_sampler = slot;
                comp.add_msl_resource_binding(b);
            }
            // The vertex streams reach the shader as [[stage_in]]; their
            // buffer indices are the pipeline's vertex descriptor's
            // (MetalBindings::kVertexStream / kInstanceStream).
            msl = comp.compile();
            return true;
        } catch (const std::exception& e) {
            error = std::string(vertex ? "vertex" : "fragment") + " MSL translation failed: " + e.what();
            return false;
        }
    }

#else

    bool Preprocess(const std::string&, bool, std::string&) { return false; }
    bool Available() { return false; }
    bool Compile(const std::string&, const std::string&, const std::string& label, Program& out) {
        out = Program{};
        out.error = label + ": built without SHADER_PACK_SPIRV";
        return false;
    }
    bool ToMetal(const std::vector<uint32_t>&, bool, uint32_t, uint32_t, std::string&, std::string& error) {
        error = "built without SHADER_PACK_SPIRV";
        return false;
    }

#endif

} // namespace Shaders::PackCompiler
