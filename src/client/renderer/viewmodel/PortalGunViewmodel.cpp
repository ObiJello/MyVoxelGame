// File: src/client/renderer/viewmodel/PortalGunViewmodel.cpp
// See header for scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalGunViewmodel.hpp"
#include "GltfLoader.hpp"
#include "../backend/RenderBackend.hpp"
#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif
#include "common/core/Log.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "../../../ext/stb_image/stb_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    PortalGunViewmodel g_portalGunViewmodel;

    namespace {
        constexpr const char* kGlbPath    = "assets/models/viewmodel/v_portalgun_animated.glb";
        constexpr const char* kTextureDir = "assets/textures/viewmodel/portalgun/";

        // Skinned vertex layout: pos3 + uv2 + normal3 + joints4(ushort) + weights4 = 56 bytes.
        // Joints arrive as ushort but we upload them as floats for portability
        // across backends — at 56 bytes total it's still cheap.
        VertexLayout BuildSkinnedLayout() {
            VertexLayout l;
            l.stride = sizeof(Gltf::Vertex);  // 56
            l.attributes = {
                {0, 3, 0,                                       false, AttribType::Float}, // POSITION
                {1, 2, (uint32_t)offsetof(Gltf::Vertex, u),     false, AttribType::Float}, // UV
                {2, 3, (uint32_t)offsetof(Gltf::Vertex, nx),    false, AttribType::Float}, // NORMAL
                // JOINTS_0 — 4 × USHORT. Mark normalized=false so the
                // vertex shader reads them as raw integer indices via
                // floatBitsToUint after casting.
                // Source ships ≤ 88 joints per skin in our model so
                // ushort range is plenty.
                {3, 4, (uint32_t)offsetof(Gltf::Vertex, joints), false, AttribType::UByte},
                {4, 4, (uint32_t)offsetof(Gltf::Vertex, weights), false, AttribType::Float},
            };
            return l;
        }

        // Map glTF material name to the converted-from-VTF PNG basename.
        std::string MaterialNameToTextureBasename(const std::string& matName) {
            std::string name = matName;
            if (name.size() >= 4 && name[name.size() - 4] == '.') {
                bool allDigits = true;
                for (size_t i = name.size() - 3; i < name.size(); ++i) {
                    if (name[i] < '0' || name[i] > '9') { allDigits = false; break; }
                }
                if (allDigits) name.resize(name.size() - 4);
            }
            return name + ".png";
        }

        bool IsGlassMaterial(const std::string& m) {
            return m.find("glass") != std::string::npos;
        }
        // The glb contains TWO v_hands primitives:
        //   • mesh[0] prim[0] mat='v_hands'      — the gun-attached
        //     forearm/wrist that bridges the grip to the player's arm.
        //     Skinned to the gun's armature (skin 0), animates with
        //     the gun. WE WANT THIS — without it there's a visible
        //     gap behind the grip that exposes the HDR clear colour
        //     during the @fire1 animation.
        //   • mesh[1] prim[0] mat='v_hands.001'  — Source's standalone
        //     V_hands_ARM rig with wide-symmetric arms posed for the
        //     player's body, not the gun. Floats detached from the
        //     gun in our renderer because we don't drive that rig.
        //     SKIP THIS.
        // Blender's exporter appends ".001" to disambiguate the second
        // material reference to the same name, so suffix-match is the
        // most direct way to tell them apart.
        bool IsHandsMaterial(const std::string& m) {
            // Match "v_hands.NNN" suffix (Blender duplicate marker).
            return m.size() >= 4 && m[m.size() - 4] == '.'
                && std::isdigit((unsigned char)m[m.size() - 3])
                && std::isdigit((unsigned char)m[m.size() - 2])
                && std::isdigit((unsigned char)m[m.size() - 1])
                && m.find("v_hands") != std::string::npos;
        }

        // We need MORE than 64 bones — Portal's v_portalgun skin has 45,
        // v_hands has 43, and the shader binds a single palette per
        // draw. 96 is a safe round number that fits comfortably in the
        // GL 3.3 uniform limit (1024 vec4s = 256 mat4s).
        constexpr int kMaxBones = 96;

        constexpr const char* kSkinnedVS = R"GLSL(
#version 330 core
layout(location = 0) in vec3  aPos;
layout(location = 1) in vec2  aUV;
layout(location = 2) in vec3  aNormal;
layout(location = 3) in vec4  aJoints;
layout(location = 4) in vec4  aWeights;

uniform mat4  uMVP;
uniform mat4  uModel;
uniform mat4  uBones[96];
uniform float uUseSkin;

out vec2 vUV;
out vec3 vNormalCam;

void main() {
    vec4 skinned;
    vec3 nrm;
    if (uUseSkin > 0.5) {
        int i0 = int(aJoints.x);
        int i1 = int(aJoints.y);
        int i2 = int(aJoints.z);
        int i3 = int(aJoints.w);
        // Normalise weights — SourceIO can emit weights that don't
        // quite sum to 1, which collapses vertices to w≈0.
        float wSum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
        vec4  w    = aWeights / max(wSum, 1e-4);
        mat4 skin = w.x * uBones[i0]
                  + w.y * uBones[i1]
                  + w.z * uBones[i2]
                  + w.w * uBones[i3];
        skinned = skin * vec4(aPos, 1.0);
        nrm     = normalize(mat3(skin) * aNormal);
    } else {
        skinned = vec4(aPos, 1.0);
        nrm     = aNormal;
    }
    gl_Position = uMVP * skinned;
    vUV         = aUV;
    vNormalCam  = mat3(uModel) * nrm;
}
)GLSL";

        constexpr const char* kSkinnedFS = R"GLSL(
#version 330 core
uniform sampler2D uDiffuse;
uniform float     uAlphaCutoff;
uniform vec3      uKeyDir;
uniform float     uKeyIntensity;
uniform float     uAmbient;
in vec2 vUV;
in vec3 vNormalCam;
out vec4 FragColor;
void main() {
    vec4 t = texture(uDiffuse, vUV);
    if (uAlphaCutoff > 0.0 && t.a < uAlphaCutoff) discard;
    vec3  n     = normalize(vNormalCam);
    float ndl   = max(dot(n, -normalize(uKeyDir)), 0.0);
    float halfL = pow(ndl * 0.5 + 0.5, 2.0);
    float shade = clamp(uAmbient + uKeyIntensity * halfL, 0.0, 1.5);
    FragColor   = vec4(t.rgb * shade, t.a);
}
)GLSL";

        // Slerp between two quats — glTF spec says LINEAR for quats is
        // actually spherical-linear.
        glm::quat Slerp(const glm::quat& a, const glm::quat& b, float t) {
            return glm::slerp(a, b, t);
        }

    } // namespace

    TextureHandle PortalGunViewmodel::LoadTextureCached(const std::string& assetBasename) {
        auto it = m_textureCache.find(assetBasename);
        if (it != m_textureCache.end()) return it->second;

        const std::string fullPath =
            PlatformMain::GetAssetPath(std::string(kTextureDir) + assetBasename);
        int w = 0, h = 0, ch = 0;
        unsigned char* pixels = stbi_load(fullPath.c_str(), &w, &h, &ch, 4);
        if (!pixels) {
            Log::Warning("[PortalGunViewmodel] tex load fail: %s", fullPath.c_str());
            m_textureCache[assetBasename] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }
        TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);
        if (t != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
            g_renderBackend->SetTextureWrap(t, TextureWrap::Repeat, TextureWrap::Repeat);
            g_renderBackend->GenerateMipmaps(t);
        }
        m_textureCache[assetBasename] = t;
        Log::Info("[PortalGunViewmodel]   tex %s (%dx%d)", assetBasename.c_str(), w, h);
        return t;
    }

    bool PortalGunViewmodel::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) return false;

        // On Vulkan, the viewmodel needs the portal pipeline layout
        // (with the BonesUBO at binding=2). On OpenGL, fall back to
        // the inline GLSL source. Both load the same uniforms by name.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal(
                "shaders/viewmodel_skinned.vert",
                "shaders/viewmodel_skinned.frag");
            // Tell the backend the viewmodel uses our 52-byte skinned
            // vertex layout (pos3 + uv2 + normal3 + joints4u8 + weights4f).
            // Without this, pipeline creation falls back to the 24-byte
            // block layout, the shader's attribute slots 3+4 are missing
            // their bindings, and VkPipeline creation fails with -3.
            if (m_shader != INVALID_SHADER) {
                vk->RegisterShaderVertexLayout(m_shader, BuildSkinnedLayout());
            }
#endif
        } else {
            m_shader = g_renderBackend->CreateShader(kSkinnedVS, kSkinnedFS);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[PortalGunViewmodel] skinned shader compile failed");
            return false;
        }

        const std::string glbPath = PlatformMain::GetAssetPath(kGlbPath);
        m_model = Gltf::LoadGLB(glbPath);
        if (m_model.primitives.empty()) {
            Log::Warning("[PortalGunViewmodel] glb empty: %s", glbPath.c_str());
            return false;
        }
        m_workingNodes = m_model.nodes;  // mutable copy for animation evaluation

        // Find the fire clip by name (Source-prefixed with "@").
        // NOTE: We do NOT auto-play `@idle`. Source's @idle clip
        // contains a single keyframe per bone with its absolute-pose
        // values, which often differ from the glTF bind pose. Playing
        // it produces a wildly different pose (e.g. the spine root
        // translates from y=+0.735 to y=-0.31 with a 90° yaw) that
        // sends the gun off-screen. The actual visible "neutral" pose
        // is the glTF bind pose itself, which is what m_workingNodes
        // already holds. So default to "no clip playing" → bind pose.
        for (size_t i = 0; i < m_model.animations.size(); ++i) {
            const std::string& n = m_model.animations[i].name;
            if (n == "@fire1") m_fireClip = (int)i;
        }

        // Upload mesh buffers per primitive.
        const VertexLayout layout = BuildSkinnedLayout();
        for (const auto& prim : m_model.primitives) {
            DrawCall dc;
            dc.indexCount = (uint32_t)prim.indices.size();
            dc.isGlass    = IsGlassMaterial(prim.materialName);
            dc.skip       = IsHandsMaterial(prim.materialName);
            // Skin index comes from the owning node — primitives copied
            // out of the glTF hierarchy carry their node's skin ref.
            if (prim.nodeIndex >= 0 && prim.nodeIndex < (int)m_model.nodes.size()) {
                dc.skinIndex = m_model.nodes[prim.nodeIndex].skinIndex;
            }

            dc.vbo = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex,
                prim.vertices.size() * sizeof(Gltf::Vertex),
                prim.vertices.data(), BufferAccess::Static);
            dc.ibo = g_renderBackend->CreateBuffer(
                BufferUsage::Index,
                prim.indices.size() * sizeof(uint32_t),
                prim.indices.data(), BufferAccess::Static);
            dc.mesh = g_renderBackend->CreateMesh(dc.vbo, dc.ibo, layout);
            dc.texture = LoadTextureCached(MaterialNameToTextureBasename(prim.materialName));
            m_drawCalls.push_back(dc);
        }

        m_initialized = true;
        Log::Info("[PortalGunViewmodel] Initialized — %zu draw calls, idle=%d fire=%d",
                  m_drawCalls.size(), m_idleClip, m_fireClip);
        return true;
    }

    void PortalGunViewmodel::Shutdown() {
        if (!g_renderBackend) { m_initialized = false; return; }
        for (auto& dc : m_drawCalls) {
            if (dc.mesh != INVALID_MESH)    g_renderBackend->DestroyMesh(dc.mesh);
            if (dc.vbo  != INVALID_BUFFER)  g_renderBackend->DestroyBuffer(dc.vbo);
            if (dc.ibo  != INVALID_BUFFER)  g_renderBackend->DestroyBuffer(dc.ibo);
        }
        m_drawCalls.clear();
        std::unordered_set<TextureHandle> done;
        for (auto& [_, t] : m_textureCache) {
            if (t != INVALID_TEXTURE && !done.count(t)) {
                g_renderBackend->DestroyTexture(t); done.insert(t);
            }
        }
        m_textureCache.clear();
        if (m_shader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
        }
        m_initialized = false;
    }

    void PortalGunViewmodel::PlayClip(int clipIndex, bool loop) {
        if (clipIndex < 0 || clipIndex >= (int)m_model.animations.size()) return;
        m_currentClip = clipIndex;
        m_loopCurrent = loop;
        m_animTime    = 0.0f;
    }

    void PortalGunViewmodel::OnFire() {
        // Source's `@fire1` is a 15-frame, ~0.625s clip — play once,
        // then fall back to idle. Per-shot retrigger restarts the clip.
        if (m_fireClip >= 0) PlayClip(m_fireClip, /*loop=*/false);
    }

    void PortalGunViewmodel::SampleAnimation() {
        // Always reset to bind pose first so when no clip is playing
        // (m_currentClip == -1) we hold the neutral pose, and when a
        // clip's channels don't cover every bone those bones still
        // appear in their bind position rather than drifting from a
        // previous clip.
        for (size_t i = 0; i < m_workingNodes.size(); ++i) {
            m_workingNodes[i].translation = m_model.nodes[i].translation;
            m_workingNodes[i].rotation    = m_model.nodes[i].rotation;
            m_workingNodes[i].scale       = m_model.nodes[i].scale;
        }
        if (m_currentClip < 0) return;
        const Gltf::Animation& anim = m_model.animations[m_currentClip];

        const float t = m_animTime;
        for (const auto& ch : anim.channels) {
            if (ch.targetNode < 0 || ch.targetNode >= (int)m_workingNodes.size()) continue;
            if (ch.samplerIdx < 0 || ch.samplerIdx >= (int)anim.samplers.size()) continue;
            const Gltf::AnimSampler& smp = anim.samplers[ch.samplerIdx];
            if (smp.input.empty()) continue;

            // Locate keyframe pair bracketing `t`.
            size_t i1 = 0;
            while (i1 < smp.input.size() && smp.input[i1] < t) ++i1;
            size_t i0;
            float  alpha;
            if (i1 == 0) {
                i0 = 0; i1 = 0; alpha = 0.0f;
            } else if (i1 >= smp.input.size()) {
                i0 = i1 = smp.input.size() - 1; alpha = 0.0f;
            } else {
                i0 = i1 - 1;
                float t0 = smp.input[i0], t1 = smp.input[i1];
                alpha = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
                if (smp.isStep) alpha = 0.0f;
            }

            const float* v0 = &smp.output[i0 * (size_t)smp.componentsPerKey];
            const float* v1 = &smp.output[i1 * (size_t)smp.componentsPerKey];

            Gltf::Node& n = m_workingNodes[ch.targetNode];
            switch (ch.path) {
                case 0: { // translation
                    glm::vec3 a(v0[0], v0[1], v0[2]);
                    glm::vec3 b(v1[0], v1[1], v1[2]);
                    n.translation = glm::mix(a, b, alpha);
                } break;
                case 1: { // rotation (glTF [x,y,z,w]; glm::quat (w,x,y,z))
                    glm::quat a(v0[3], v0[0], v0[1], v0[2]);
                    glm::quat b(v1[3], v1[0], v1[1], v1[2]);
                    n.rotation = Slerp(a, b, alpha);
                } break;
                case 2: { // scale
                    glm::vec3 a(v0[0], v0[1], v0[2]);
                    glm::vec3 b(v1[0], v1[1], v1[2]);
                    n.scale = glm::mix(a, b, alpha);
                } break;
                default: break;
            }
        }
    }

    glm::mat4 PortalGunViewmodel::WorldMatrix(int nodeIndex) const {
        if (nodeIndex < 0 || nodeIndex >= (int)m_workingNodes.size()) return glm::mat4(1.0f);
        const Gltf::Node& n = m_workingNodes[nodeIndex];
        glm::mat4 local = glm::translate(glm::mat4(1.0f), n.translation)
                        * glm::mat4_cast(n.rotation)
                        * glm::scale(glm::mat4(1.0f), n.scale);
        if (n.parent < 0) return local;
        return WorldMatrix(n.parent) * local;
    }

    void PortalGunViewmodel::ComputeBoneMatrices(int skinIndex,
                                                 std::vector<glm::mat4>& out) const {
        if (skinIndex < 0 || skinIndex >= (int)m_model.skins.size()) {
            out.assign(1, glm::mat4(1.0f));
            return;
        }
        const Gltf::Skin& sk = m_model.skins[skinIndex];

        // Per spec: bone matrix = jointWorld × inverseBindMatrix
        // (the optional inverse(rootWorld) compensation is a no-op when
        // the root sits at identity, which is true for first-person
        // viewmodels — we own the camera-space transform separately).
        const size_t n = sk.jointNodeIndices.size();
        out.resize(std::min<size_t>(n, kMaxBones));
        for (size_t i = 0; i < out.size(); ++i) {
            const glm::mat4 w = WorldMatrix(sk.jointNodeIndices[i]);
            out[i] = w * sk.inverseBindMatrices[i];
        }
        // Pad any unused palette slots with identity so the shader is safe.
        for (size_t i = out.size(); i < (size_t)kMaxBones; ++i) {
            out.push_back(glm::mat4(1.0f));
        }
    }

    void PortalGunViewmodel::Render(float aspect, float dt) {
        if (!m_initialized || !g_renderBackend) return;
        if (m_drawCalls.empty()) return;

        m_time += dt;

        // Advance current clip. When a non-looping clip finishes we
        // STOP sampling (m_currentClip = -1) so the next SampleAnimation
        // call returns to the bind pose. We deliberately don't transition
        // into @idle because Source's @idle keyframes encode a different
        // pose than the bind pose — playing them displaces the gun off-
        // screen (see comment in Initialize about why we don't auto-play
        // @idle).
        if (m_currentClip >= 0) {
            const float dur = m_model.animations[m_currentClip].duration;
            m_animTime += dt;
            if (dur > 0.0f && m_animTime >= dur) {
                if (m_loopCurrent) {
                    m_animTime = std::fmod(m_animTime, dur);
                } else {
                    m_currentClip = -1;  // stop → bind pose
                    m_animTime    = 0.0f;
                }
            }
        }
        SampleAnimation();

        // Viewmodel projection + transform — same as the static version.
        const float fovY = glm::radians(54.0f);
        const glm::mat4 proj = glm::perspective(fovY, aspect, 0.01f, 8.0f);
        const glm::mat4 view = glm::mat4(1.0f);

        const float bobY = std::sin(m_time * 0.8f * 6.2831853f) * 0.004f;
        const float bobX = std::cos(m_time * 0.6f * 6.2831853f) * 0.003f;

        glm::mat4 model(1.0f);
        model = glm::translate(model, glm::vec3(0.18f + bobX, -0.16f + bobY, -0.32f));
        model = glm::rotate(model, glm::radians(180.0f), glm::vec3(0,1,0));
        model = glm::rotate(model, glm::radians(-3.0f),  glm::vec3(1,0,0));
        model = glm::translate(model, glm::vec3(0.11f, -0.92f, 0.03f));

        const glm::mat4 mvp = proj * view * model;

        // Depth-clear so the gun sits on top regardless of world depth.
        g_renderBackend->Clear(false, true, false);

        PipelineState opaque;
        opaque.depthTestEnabled  = true;
        opaque.depthWriteEnabled = true;
        opaque.depthCompareOp    = CompareOp::LessEqual;
        opaque.colorWriteEnabled = true;
        opaque.blendEnabled      = false;
        opaque.cullMode          = CullMode::None;
        opaque.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(opaque);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformMat4 (m_shader, "uMVP",   mvp);
        g_renderBackend->SetUniformMat4 (m_shader, "uModel", model);
        g_renderBackend->SetUniformVec3 (m_shader, "uKeyDir",
            glm::normalize(glm::vec3(0.2f, -0.8f, -0.5f)));
        g_renderBackend->SetUniformFloat(m_shader, "uKeyIntensity", 0.95f);
        g_renderBackend->SetUniformFloat(m_shader, "uAmbient",      0.35f);

        std::vector<std::vector<glm::mat4>> bonePalettes(m_model.skins.size());
        std::vector<bool>                   computed(m_model.skins.size(), false);

        auto BindSkin = [&](int skinIdx) {
            if (skinIdx < 0) {
                g_renderBackend->SetUniformFloat(m_shader, "uUseSkin", 0.0f);
                return;
            }
            if (!computed[skinIdx]) {
                ComputeBoneMatrices(skinIdx, bonePalettes[skinIdx]);
                computed[skinIdx] = true;
            }
            g_renderBackend->SetUniformFloat(m_shader, "uUseSkin", 1.0f);
            const auto& palette = bonePalettes[skinIdx];
            char buf[32];
            for (size_t i = 0; i < palette.size(); ++i) {
                std::snprintf(buf, sizeof(buf), "uBones[%zu]", i);
                g_renderBackend->SetUniformMat4(m_shader, buf, palette[i]);
            }
        };

        for (const auto& dc : m_drawCalls) {
            if (dc.skip || dc.isGlass) continue;
            if (dc.texture == INVALID_TEXTURE) continue;
            BindSkin(dc.skinIndex);
            g_renderBackend->BindTexture(dc.texture, 0);
            g_renderBackend->SetUniformInt  (m_shader, "uDiffuse", 0);
            g_renderBackend->SetUniformFloat(m_shader, "uAlphaCutoff", 0.5f);
            g_renderBackend->DrawIndexed(dc.mesh, dc.indexCount);
        }

        bool hasGlass = false;
        for (const auto& dc : m_drawCalls) if (dc.isGlass && !dc.skip) { hasGlass = true; break; }
        if (hasGlass) {
            PipelineState glass = opaque;
            glass.depthWriteEnabled = false;
            glass.blendEnabled      = true;
            glass.srcBlendFactor    = BlendFactor::SrcAlpha;
            glass.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
            glass.cullMode          = CullMode::None;
            g_renderBackend->SetPipelineState(glass);
            g_renderBackend->BindShader(m_shader);
            g_renderBackend->SetUniformMat4 (m_shader, "uMVP",   mvp);
            g_renderBackend->SetUniformMat4 (m_shader, "uModel", model);
            g_renderBackend->SetUniformVec3 (m_shader, "uKeyDir",
                glm::normalize(glm::vec3(0.2f, -0.8f, -0.5f)));
            g_renderBackend->SetUniformFloat(m_shader, "uKeyIntensity", 0.95f);
            g_renderBackend->SetUniformFloat(m_shader, "uAmbient",      0.45f);
    
            for (const auto& dc : m_drawCalls) {
                if (!dc.isGlass || dc.skip) continue;
                if (dc.texture == INVALID_TEXTURE) continue;
                BindSkin(dc.skinIndex);
                g_renderBackend->BindTexture(dc.texture, 0);
                g_renderBackend->SetUniformInt  (m_shader, "uDiffuse", 0);
                g_renderBackend->SetUniformFloat(m_shader, "uAlphaCutoff", 0.0f);
                g_renderBackend->DrawIndexed(dc.mesh, dc.indexCount);
            }
        }

        g_renderBackend->UnbindMesh();
    }

} // namespace Render

#endif // ENABLE_PORTAL_GUN
