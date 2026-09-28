// File: src/client/renderer/post/PostChain.cpp
#include "PostChain.hpp"

#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <fstream>
#include <sstream>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        using Json = nlohmann::ordered_json;   // the uniform blocks keep file order

        // The block vertex layout (24 bytes): NDC position, GL-convention
        // texture coordinate, an unused colour — as EntityOutline's quad.
        struct QuadVert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(QuadVert) == 24, "must match GetBlockVertexLayout");

        // "minecraft:core/screenquad" -> "screenquad"; "minecraft:post/blit" -> "blit".
        std::string ShaderBaseName(const std::string& id) {
            std::string path = id;
            if (const size_t colon = path.find(':'); colon != std::string::npos) path = path.substr(colon + 1);
            if (const size_t slash = path.rfind('/'); slash != std::string::npos) path = path.substr(slash + 1);
            return path;
        }

        // MC's uniform types, packed std140-style into one flat array.
        bool PackUniform(const Json& entry, std::array<float, 16>& out, size_t& cursor, std::string& error) {
            const std::string type = entry.value("type", std::string());
            size_t count = 0, align = 1;
            if (type == "float" || type == "int")  { count = 1; align = 1; }
            else if (type == "vec2" || type == "ivec2") { count = 2; align = 2; }
            else if (type == "vec3" || type == "ivec3") { count = 3; align = 4; }
            else if (type == "vec4" || type == "ivec4") { count = 4; align = 4; }
            else { error = "unsupported uniform type '" + type + "'"; return false; }
            cursor = (cursor + align - 1) / align * align;
            if (cursor + count > out.size()) { error = "uniforms exceed sixteen floats"; return false; }
            const Json& value = entry.contains("value") ? entry["value"] : Json();
            for (size_t i = 0; i < count; ++i) {
                float v = 0.0f;
                if (value.is_array() && i < value.size() && value[i].is_number()) v = value[i].get<float>();
                else if (count == 1 && value.is_number()) v = value.get<float>();
                out[cursor + i] = v;
            }
            cursor += count;
            return true;
        }

        void RefreshTextureState(RenderTargetHandle target) {
            RenderBackend& b = *g_renderBackend;
            const TextureHandle tex = b.GetRenderTargetColorTexture(target);
            if (tex == INVALID_TEXTURE) return;
            b.SetTextureFilter(tex, TextureFilter::Linear, TextureFilter::Linear);
            b.SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }

    } // namespace

    // ── PostChainConfig ─────────────────────────────────────────────────────

    bool PostChainConfig::Parse(const std::string& text, PostChainConfig& out, std::string& error) {
        Json root;
        try {
            root = Json::parse(text);
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        if (!root.is_object()) { error = "not an object"; return false; }

        if (root.contains("targets")) {
            const Json& targets = root["targets"];
            if (!targets.is_object()) { error = "'targets' is not an object"; return false; }
            for (auto it = targets.begin(); it != targets.end(); ++it) {
                Target t;
                t.name = it.key();
                if (it.value().is_object()) {
                    t.width  = it.value().value("width", 0);
                    t.height = it.value().value("height", 0);
                }
                out.targets.push_back(std::move(t));
            }
        }

        const auto isTarget = [&out](const std::string& name) {
            if (name == PostChain::kMainTarget) return true;
            for (const Target& t : out.targets) if (t.name == name) return true;
            return false;
        };

        if (!root.contains("passes") || !root["passes"].is_array()) { error = "no 'passes'"; return false; }
        for (const Json& p : root["passes"]) {
            Pass pass;
            pass.vertexShader   = p.value("vertex_shader", std::string());
            pass.fragmentShader = p.value("fragment_shader", std::string());
            pass.output         = p.value("output", std::string());
            if (pass.vertexShader.empty() || pass.fragmentShader.empty()) {
                error = "a pass without shaders";
                return false;
            }
            if (!isTarget(pass.output)) { error = "unknown output target '" + pass.output + "'"; return false; }
            if (p.contains("inputs")) {
                for (const Json& in : p["inputs"]) {
                    Input input;
                    input.samplerName = in.value("sampler_name", std::string());
                    if (!in.contains("target")) {
                        // A texture input (MC's "location"): no chain here uses one.
                        error = "input '" + input.samplerName + "' is not a target";
                        return false;
                    }
                    input.target   = in.value("target", std::string());
                    input.bilinear = in.value("bilinear", false);
                    if (!isTarget(input.target)) { error = "unknown input target '" + input.target + "'"; return false; }
                    pass.inputs.push_back(std::move(input));
                }
            }
            if (pass.inputs.size() > 2) { error = "more than two inputs in one pass"; return false; }
            if (p.contains("uniforms") && p["uniforms"].is_object()) {
                size_t cursor = 0;
                for (auto block = p["uniforms"].begin(); block != p["uniforms"].end(); ++block) {
                    cursor = (cursor + 3) / 4 * 4;   // a std140 block starts on a vec4
                    if (!block.value().is_array()) continue;
                    for (const Json& entry : block.value()) {
                        if (!PackUniform(entry, pass.params, cursor, error)) return false;
                    }
                }
            }
            out.passes.push_back(std::move(pass));
        }
        return true;
    }

    // ── PostChain ───────────────────────────────────────────────────────────

    PostChain::PostChain(std::string id, PostChainConfig config)
        : m_id(std::move(id)), m_config(std::move(config)) {}

    PostChain::~PostChain() { ReleaseTargets(); }

    RenderTargetHandle PostChain::MakeTarget(int width, int height) {
        RenderTargetDesc desc;
        desc.width = width;
        desc.height = height;
        desc.colorFormat = TextureFormat::RGBA8;   // MC's post targets: RGBA8_UNORM
        return g_renderBackend->CreateRenderTarget(desc);
    }

    void PostChain::ConfigureTexture(RenderTargetHandle target) { RefreshTextureState(target); }

    void PostChain::ReleaseTargets() {
        if (!g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;
        for (TargetSet& set : m_sets) {
            for (auto& [name, rt] : set.targets) {
                if (rt != INVALID_RENDER_TARGET) b.DestroyRenderTarget(rt);
            }
            set.targets.clear();
            if (set.mainCopy != INVALID_RENDER_TARGET) b.DestroyRenderTarget(set.mainCopy);
            set.mainCopy = INVALID_RENDER_TARGET;
            set.width = set.height = 0;
        }
    }

    bool PostChain::EnsureTargets(TargetSet& set, int width, int height) {
        RenderBackend& b = *g_renderBackend;
        const auto sizeOf = [&](const PostChainConfig::Target& t) {
            return glm::ivec2(t.width > 0 ? t.width : width, t.height > 0 ? t.height : height);
        };
        if (set.mainCopy == INVALID_RENDER_TARGET) {
            set.mainCopy = MakeTarget(width, height);
            if (set.mainCopy == INVALID_RENDER_TARGET) return false;
            ConfigureTexture(set.mainCopy);
            for (const PostChainConfig::Target& t : m_config.targets) {
                const glm::ivec2 size = sizeOf(t);
                const RenderTargetHandle rt = MakeTarget(size.x, size.y);
                if (rt == INVALID_RENDER_TARGET) return false;
                ConfigureTexture(rt);
                set.targets[t.name] = rt;
            }
            set.width = width;
            set.height = height;
            return true;
        }
        if (set.width != width || set.height != height) {
            // MC PostChain resizes its targets with the main one.
            b.ResizeRenderTarget(set.mainCopy, width, height);
            ConfigureTexture(set.mainCopy);
            for (const PostChainConfig::Target& t : m_config.targets) {
                if (t.width > 0 && t.height > 0) continue;   // a fixed size never follows the window
                const glm::ivec2 size = sizeOf(t);
                b.ResizeRenderTarget(set.targets[t.name], size.x, size.y);
                ConfigureTexture(set.targets[t.name]);
            }
            set.width = width;
            set.height = height;
        }
        return true;
    }

    bool PostChain::Process(int width, int height, MeshHandle quad, const std::vector<ShaderHandle>& shaders,
                            float menuBlurRadius) {
        if (!g_renderBackend || quad == INVALID_MESH || shaders.size() != m_config.passes.size()) return false;
        RenderBackend& b = *g_renderBackend;

        // The other set from last time: the frame before may still be
        // reading this one's images (Vulkan frames overlap on the GPU).
        TargetSet& set = m_sets[static_cast<size_t>(m_nextSet)];
        m_nextSet ^= 1;
        if (!EnsureTargets(set, width, height)) {
            Log::Warning("[PostChain] %s: render targets unavailable", m_id.c_str());
            return false;
        }

        const auto targetSize = [&](const std::string& name) {
            for (const PostChainConfig::Target& t : m_config.targets) {
                if (t.name == name) {
                    return glm::vec2(static_cast<float>(t.width > 0 ? t.width : width),
                                     static_cast<float>(t.height > 0 ? t.height : height));
                }
            }
            return glm::vec2(static_cast<float>(width), static_cast<float>(height));
        };

        // Post passes: no depth, no blending, no culling (MC's post
        // pipelines have none of the three).
        PipelineState flat;
        flat.depthTestEnabled  = false;
        flat.depthWriteEnabled = false;
        flat.blendEnabled      = false;
        flat.cullMode          = CullMode::None;
        flat.primitiveType     = PrimitiveType::Triangles;

        bool mainCopied = false;
        for (size_t i = 0; i < m_config.passes.size(); ++i) {
            const PostChainConfig::Pass& pass = m_config.passes[i];
            const ShaderHandle shader = shaders[i];

            TextureHandle tex[2]  = {INVALID_TEXTURE, INVALID_TEXTURE};
            glm::vec2     size[2] = {glm::vec2(1.0f), glm::vec2(1.0f)};
            float         bilinear[2] = {0.0f, 0.0f};
            for (const PostChainConfig::Input& input : pass.inputs) {
                const int slot = input.samplerName == "In" ? 0 : 1;
                RenderTargetHandle source = INVALID_RENDER_TARGET;
                if (input.target == kMainTarget) {
                    // The frame as it stands — copied once, and again only
                    // after a pass has drawn onto it.
                    if (!mainCopied) {
                        b.BindRenderTarget(INVALID_RENDER_TARGET);
                        if (!b.CopyFramebufferToRenderTarget(set.mainCopy)) {
                            Log::Warning("[PostChain] %s: the frame could not be copied", m_id.c_str());
                            return false;
                        }
                        mainCopied = true;
                    }
                    source = set.mainCopy;
                    size[slot] = glm::vec2(static_cast<float>(width), static_cast<float>(height));
                } else {
                    source = set.targets[input.target];
                    size[slot] = targetSize(input.target);
                }
                tex[slot] = b.GetRenderTargetColorTexture(source);
                bilinear[slot] = input.bilinear ? 1.0f : 0.0f;
            }
            if (tex[1] == INVALID_TEXTURE) {
                tex[1] = tex[0];
                size[1] = size[0];
            }

            const bool toMain = pass.output == kMainTarget;
            const glm::vec2 outSize = toMain ? glm::vec2(static_cast<float>(width), static_cast<float>(height))
                                             : targetSize(pass.output);
            b.BindRenderTarget(toMain ? INVALID_RENDER_TARGET : set.targets[pass.output]);
            b.SetViewport(0, 0, static_cast<int>(outSize.x), static_cast<int>(outSize.y));
            b.SetPipelineState(flat);
            b.BindShader(shader);
            b.BindTexture(tex[0], 0);
            b.BindTexture(tex[1], 1);
            b.SetUniformInt(shader, "uInSampler", 0);
            b.SetUniformInt(shader, "uAuxSampler", 1);
            b.SetUniformVec2(shader, "uInSize", size[0]);
            b.SetUniformVec2(shader, "uAuxSize", size[1]);
            b.SetUniformVec2(shader, "uOutSize", outSize);
            b.SetUniformFloat(shader, "uInBilinear", bilinear[0]);
            b.SetUniformFloat(shader, "uAuxBilinear", bilinear[1]);
            b.SetUniformFloat(shader, "uMenuBlurRadius", menuBlurRadius);
            glm::mat4 params(0.0f);
            for (int c = 0; c < 4; ++c) {
                params[c] = glm::vec4(pass.params[static_cast<size_t>(c * 4 + 0)],
                                      pass.params[static_cast<size_t>(c * 4 + 1)],
                                      pass.params[static_cast<size_t>(c * 4 + 2)],
                                      pass.params[static_cast<size_t>(c * 4 + 3)]);
            }
            b.SetUniformMat4(shader, "uPostParams", params);
            b.DrawArrays(quad, 6, 0);
            b.UnbindMesh();
            if (toMain) mainCopied = false;   // main changed: a later read copies again
        }

        // Back to the frame, and no post target left bound as the extra
        // texture for whatever draws next.
        b.BindRenderTarget(INVALID_RENDER_TARGET);
        b.SetViewport(0, 0, width, height);
        b.BindTexture(INVALID_TEXTURE, 1);
        return true;
    }

    // ── PostEffects ─────────────────────────────────────────────────────────

    PostEffects& PostEffects::Get() {
        static PostEffects s_instance;
        return s_instance;
    }

    bool PostEffects::EnsureQuad() {
        if (m_quadMesh != INVALID_MESH) return true;
        if (!g_renderBackend) return false;
        RenderBackend& b = *g_renderBackend;
        const QuadVert quad[6] = {
            {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
            { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255},
            { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
            {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
            { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
            {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
        };
        m_quadVB = b.CreateBuffer(BufferUsage::Vertex, sizeof(quad), quad, BufferAccess::Static);
        if (m_quadVB != INVALID_BUFFER) m_quadMesh = b.CreateMesh(m_quadVB, INVALID_BUFFER, GetBlockVertexLayout());
        return m_quadMesh != INVALID_MESH;
    }

    ShaderHandle PostEffects::GetShader(const std::string& vertexId, const std::string& fragmentId) {
        const std::string key = vertexId + "|" + fragmentId;
        if (auto it = m_shaders.find(key); it != m_shaders.end()) return it->second;
        const std::string vert = "shaders/post_" + ShaderBaseName(vertexId) + ".vert";
        const std::string frag = "shaders/post_" + ShaderBaseName(fragmentId) + ".frag";
        // The portal pipeline layout on Vulkan: two sampler sets (In and the
        // pass's second input) plus the push constants the config rides in.
        const ShaderHandle shader = EntityEnvironment::CreateShader(vert, frag);
        m_shaders[key] = shader;
        return shader;
    }

    PostChain* PostEffects::GetChain(const std::string& id) {
        if (auto it = m_chains.find(id); it != m_chains.end()) return it->second.get();
        if (m_failed.count(id)) return nullptr;

        // "minecraft:creeper" or "creeper" -> assets/post_effect/creeper.json.
        std::string name = id;
        if (const size_t colon = name.find(':'); colon != std::string::npos) name = name.substr(colon + 1);
        const std::string path = PlatformMain::GetAssetPath("assets/post_effect/" + name + ".json");
        std::ifstream file(path);
        if (!file) {
            Log::Warning("[PostEffects] %s: %s not found", id.c_str(), path.c_str());
            m_failed.insert(id);
            return nullptr;
        }
        std::stringstream text;
        text << file.rdbuf();
        PostChainConfig config;
        std::string error;
        if (!PostChainConfig::Parse(text.str(), config, error)) {
            Log::Warning("[PostEffects] %s: %s", id.c_str(), error.c_str());
            m_failed.insert(id);
            return nullptr;
        }
        std::vector<ShaderHandle> shaders;
        for (const PostChainConfig::Pass& pass : config.passes) {
            const ShaderHandle shader = GetShader(pass.vertexShader, pass.fragmentShader);
            if (shader == INVALID_SHADER) {
                Log::Warning("[PostEffects] %s: shader %s + %s unavailable", id.c_str(),
                             pass.vertexShader.c_str(), pass.fragmentShader.c_str());
                m_failed.insert(id);
                return nullptr;
            }
            shaders.push_back(shader);
        }
        Log::Info("[PostEffects] Loaded post effect %s (%zu passes, %zu targets)", id.c_str(),
                  config.passes.size(), config.targets.size());
        m_chainShaders[id] = std::move(shaders);
        auto chain = std::make_unique<PostChain>(id, std::move(config));
        PostChain* raw = chain.get();
        m_chains[id] = std::move(chain);
        return raw;
    }

    void PostEffects::Apply(int framebufferWidth, int framebufferHeight, bool shaderPackActive) {
        // MC preparePostEffects: targets of a chain nobody asked for are
        // closed. Here a chain keeps them for a few seconds unused (MC's
        // resource pool ages its textures out the same way), so looking
        // from mob to mob does not rebuild them each time.
        constexpr int kIdleFramesBeforeRelease = 120;
        // MC applies the spectated effect only while it is active
        // (`spectatedEntityPostEffect != null && spectatedEntityEffectActive`).
        const std::string requested = m_active ? m_spectated : std::string();
        for (auto& [id, chain] : m_chains) {
            if (id == requested && !shaderPackActive) continue;
            int& idle = m_idleFrames[id];
            if (++idle == kIdleFramesBeforeRelease) chain->ReleaseTargets();
        }

        if (requested.empty() || !g_renderBackend) return;
        if (framebufferWidth <= 0 || framebufferHeight <= 0) return;
        if (shaderPackActive) {
            if (!m_loggedPackSkip) {
                m_loggedPackSkip = true;
                Log::Info("[PostEffects] %s not applied: a shader pack is active and owns post-processing "
                          "(OptiFine behaviour)", m_spectated.c_str());
            }
            return;
        }
        m_loggedPackSkip = false;
        if (!EnsureQuad()) return;

        PostChain* chain = GetChain(m_spectated);
        if (!chain) return;
        m_idleFrames[m_spectated] = 0;

        PROFILE_ZONE_N("PostEffects");
        // globals.MenuBlurRadius — the menu blur option (box_blur's radius
        // when a chain leaves its own unset).
        constexpr float kDefaultMenuBlurRadius = 5.0f;
        if (!chain->Process(framebufferWidth, framebufferHeight, m_quadMesh,
                            m_chainShaders[m_spectated], kDefaultMenuBlurRadius)) {
            // MC applyPostEffects: a chain that throws goes on the failed list.
            Log::Warning("[PostEffects] %s failed to apply; disabled until the next resource reload",
                         m_spectated.c_str());
            chain->ReleaseTargets();
            m_failed.insert(m_spectated);
        }

        // The frame's own state for what draws next (the GUI).
        RenderBackend& b = *g_renderBackend;
        const glm::vec3 fog = EnvironmentState::Get().Frame().fogColor;
        b.SetClearColor(fog.r, fog.g, fog.b, 1.0f);
        PipelineState def;
        b.SetPipelineState(def);
    }

    void PostEffects::Reload() {
        for (auto& [id, chain] : m_chains) chain->ReleaseTargets();
        m_chains.clear();
        m_chainShaders.clear();
        m_idleFrames.clear();
        m_failed.clear();
    }

    void PostEffects::Shutdown() {
        Reload();
        if (!g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;
        for (auto& [key, shader] : m_shaders) {
            if (shader != INVALID_SHADER) b.DestroyShader(shader);
        }
        m_shaders.clear();
        if (m_quadMesh != INVALID_MESH) { b.DestroyMesh(m_quadMesh); m_quadMesh = INVALID_MESH; }
        if (m_quadVB != INVALID_BUFFER) { b.DestroyBuffer(m_quadVB); m_quadVB = INVALID_BUFFER; }
        m_loggedPackSkip = false;
    }

} // namespace Render
