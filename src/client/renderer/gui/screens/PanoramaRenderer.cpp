// File: src/client/renderer/gui/screens/PanoramaRenderer.cpp
#include "PanoramaRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "Widgets.hpp"   // ApplyAlpha
#include "../GuiGraphics.hpp"
#include "../../backend/RenderBackend.hpp"
#include "../../texture/MipmapGenerator.hpp"
#include "common/core/Log.hpp"
#include "../../ext/stb_image/stb_image.h"
#include "../../ext/stb_image/stb_image_write.h"   // implementation lives in AtlasBuilder.cpp
#include "platform/GameDirectory.hpp"
#include <nlohmann/json.hpp>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <chrono>
#include <string_view>
#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    PanoramaRenderer g_panoramaRenderer;

    // Vertex layout reuses the block layout (pos3 + uv2 + color4 ubyte) so we
    // can build the mesh with GetBlockVertexLayout like BlockBreakOverlay.
    const char* PanoramaRenderer::vertexShaderSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
}
)";

    const char* PanoramaRenderer::fragmentShaderSource = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTexture;
void main() {
    FragColor = vec4(texture(uTexture, vUV).rgb, 1.0);
}
)";

    namespace {
        // Every panorama Minecraft has shipped, oldest first. Folder must
        // exist under assets/textures/gui/title/background/ to be offered.
        const PanoramaRenderer::SetInfo kSets[] = {
            {"classic", "Classic"},   // Beta 1.8 – 1.12
            {"1.13",    "1.13"},      // Update Aquatic
            {"1.14",    "1.14"},      // Village & Pillage
            {"1.15",    "1.15"},      // Buzzy Bees
            {"1.16",    "1.16"},      // Nether Update
            {"1.17",    "1.17"},      // Caves & Cliffs I
            {"1.18",    "1.18"},      // Caves & Cliffs II
            {"1.19",    "1.19"},      // The Wild Update
            {"1.20",    "1.20"},      // Trails & Tales
            {"1.21",    "1.21"},      // Tricky Trials
            {"1.21.4",  "1.21.4"},    // The Garden Awakens
            {"1.21.5",  "1.21.5"},    // Spring to Life
            {"1.21.6",  "1.21.6"},    // Chase the Skies
            {"1.21.9",  "1.21.9"},    // Mounts of Mayhem
            {"1.21.11", "1.21.11"},
            {"26.1",    "26.1"},
            {"26.2",    "26.2"},
        };

        std::string SetDir(const std::string& slug) {
            return "assets/textures/gui/title/background/" + slug + "/";
        }

        // Where a set's files live: the asset tree (through the resource
        // pack override), or the game directory for the captured set.
        std::string FacePath(const std::string& slug, const std::string& file) {
            if (slug == PanoramaRenderer::kLastWorldSet) return PanoramaRenderer::LastWorldDir() + file;
            return PlatformMain::GetAssetPath(SetDir(slug) + file);
        }

        // Loads one face texture. Returns INVALID_TEXTURE when the file is
        // missing OR is one of the 1×1 placeholder stubs in the asset dump.
        TextureHandle LoadFaceTexture(const std::string& full, bool& wasStub) {
            wasStub = false;
            if (!std::filesystem::exists(full)) return INVALID_TEXTURE;

            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) return INVALID_TEXTURE;
            if (w <= 1 || h <= 1) {
                wasStub = true;
                stbi_image_free(pixels);
                return INVALID_TEXTURE;
            }
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                // Linear filtering: the skybox is heavily magnified.
                g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return t;
        }
    } // namespace

    std::vector<PanoramaRenderer::SetInfo> PanoramaRenderer::AvailableSets() {
        std::vector<SetInfo> out;
        for (const auto& set : kSets) {
            const std::string probe =
                PlatformMain::GetAssetPath(SetDir(set.slug) + "panorama_0.png");
            if (std::filesystem::exists(probe)) out.push_back(set);
        }
        // Always offered, so it can be chosen before the first capture
        // exists; LoadSet falls back to the default until then.
        out.push_back({kLastWorldSet, "Last World"});
        return out;
    }

    bool  PanoramaRenderer::s_handoffPending = false;
    float PanoramaRenderer::s_handoffYaw = 0.0f;
    float PanoramaRenderer::s_handoffPitch = 10.0f;
    float PanoramaRenderer::s_handoffFov = 85.0f;

    void PanoramaRenderer::SetHandoff(float yawOffsetDeg, float pitchDeg, float fovDeg) {
        s_handoffPending = true;
        s_handoffYaw     = yawOffsetDeg;
        s_handoffPitch   = pitchDeg;
        s_handoffFov     = fovDeg;
    }

    void PanoramaRenderer::EaseTo(float yawOffsetDeg, float pitchDeg, float fovDeg, float seconds) {
        // The spin that shows `yawOffset` is its negative (see Render);
        // shortest way round from where the drift has got to.
        float target = -yawOffsetDeg;
        float delta  = target - m_spin;
        while (delta >  180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        m_spinFrom      = m_spin;
        m_spinTo        = m_spin + delta;
        m_easePitchFrom = m_lastPitch;
        m_easePitchTo   = pitchDeg;
        m_easeFovFrom   = m_lastFov;
        m_easeFovTo     = fovDeg;
        m_easeSeconds   = std::max(seconds, 0.05f);
        m_easeStart     = std::chrono::steady_clock::now();
        m_easeActive    = true;
        m_easeDone      = false;
        m_handoffActive = false;   // the arrival ease, if still running, yields to this one
    }

    void PanoramaRenderer::ApplyHandoff(const std::string& loadedSlug) {
        const bool take = s_handoffPending && loadedSlug == kLastWorldSet;
        s_handoffPending = false;
        m_handoffActive  = take;
        if (!take) return;
        // The view yaw is `yawOffset` past face 0, turning right positive;
        // the cube turns the other way as m_spin grows (see Render), so the
        // spin that shows it is its negative.
        m_spin = -s_handoffYaw;
        while (m_spin < 0.0f)    m_spin += 360.0f;
        while (m_spin >= 360.0f) m_spin -= 360.0f;
        m_pitchFrom    = s_handoffPitch;
        m_fovFrom      = s_handoffFov;
        m_handoffStart = std::chrono::steady_clock::now();
    }

    std::string PanoramaRenderer::LastWorldDir() {
        return (std::filesystem::path(Platform::g_gameDirectory.GetGameDirectory()) /
                "panorama" / kLastWorldSet / "").string();
    }

    std::shared_ptr<PanoramaRenderer::LastWorldFaces> PanoramaRenderer::s_memoryFaces;
    std::thread PanoramaRenderer::s_saveThread;
    TextureHandle PanoramaRenderer::s_adoptedFaces[6] = {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE,
                                                        INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE};
    int PanoramaRenderer::s_adoptedSize = 0;

    TextureHandle PanoramaRenderer::s_lastWorldTex[6] = {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE,
                                                        INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE};
    std::future<Mipmap::Image> PanoramaRenderer::s_mipJobs[6];
    bool PanoramaRenderer::s_mipUploaded[6] = {};

    void PanoramaRenderer::BeginLastWorldFace(int face, int size) {
        if (!g_renderBackend || face < 0 || face >= 6 || size <= 0) return;
        if (s_adoptedSize != size) {
            DiscardAdoptedFaces();
            s_adoptedSize = size;
        }
        if (s_adoptedFaces[face] != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(s_adoptedFaces[face]);
        if (s_mipJobs[face].valid()) s_mipJobs[face].wait();
        // Two levels reserved, both empty; the tiles fill level 0 and the
        // mip job's result fills level 1. Until then the face samples
        // plain linear, which at the hand-over's 1:1 never reaches level 1.
        TextureHandle& tex = s_adoptedFaces[face];
        tex = g_renderBackend->CreateEmptyTexture2D(size, size, TextureFormat::RGBA8, /*maxLevel=*/1);
        if (tex == INVALID_TEXTURE) return;
        g_renderBackend->SetTextureFilter(tex, TextureFilter::Linear, TextureFilter::Linear);
        g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        g_renderBackend->SetTextureAnisotropy(tex, 16.0f);
        s_lastWorldTex[face] = tex;
        s_mipUploaded[face]  = false;
    }

    void PanoramaRenderer::UploadLastWorldRegion(int face, int x, int y, int w, int h, const uint8_t* rgba) {
        if (!g_renderBackend || face < 0 || face >= 6 || !rgba) return;
        if (s_adoptedFaces[face] == INVALID_TEXTURE) return;
        // ~8 MB a tile on the immediate path: a few milliseconds, inside a
        // frame. A whole face at once (38 MB) was 17-28 ms and dropped one.
        g_renderBackend->UploadTextureRegionNow(s_adoptedFaces[face], 0, x, y, w, h, rgba);
    }

    void PanoramaRenderer::FinishLastWorldFace(int face, std::shared_ptr<LastWorldFaces> faces) {
        if (!faces || face < 0 || face >= 6) return;
        if (s_adoptedFaces[face] == INVALID_TEXTURE) return;
        if (s_mipJobs[face].valid()) s_mipJobs[face].wait();
        // The half-size level, off the frame. The title's 85° lens minifies
        // the face by ~1.3×; without it that minification sparkled and
        // read as "not the world" right at the switch. `faces` keeps the
        // buffer alive for the job.
        s_mipJobs[face] = std::async(std::launch::async, [faces, face]() {
            Mipmap::Image level0;
            level0.width  = faces->size;
            level0.height = faces->size;
            level0.pixels = faces->rgba[static_cast<size_t>(face)];   // the filter consumes its input
            std::vector<Mipmap::Image> levels = Mipmap::GenerateMipLevels(
                std::move(level0), /*maxLevel=*/1, Mipmap::Strategy::Mean, 0.0f, /*isItemTexture=*/false);
            return levels.size() > 1 ? std::move(levels[1]) : Mipmap::Image{};
        });
    }

    void PanoramaRenderer::SetLastWorldFace(int face, std::shared_ptr<LastWorldFaces> faces) {
        // The one-shot form: for a caller with a whole face in hand.
        if (!faces || face < 0 || face >= 6) return;
        const int size = faces->size;
        const std::vector<uint8_t>& rgba = faces->rgba[static_cast<size_t>(face)];
        if (size <= 0 || rgba.size() != static_cast<size_t>(size) * static_cast<size_t>(size) * 4u) return;
        BeginLastWorldFace(face, size);
        UploadLastWorldRegion(face, 0, 0, size, size, rgba.data());
        FinishLastWorldFace(face, std::move(faces));
    }

    void PanoramaRenderer::PumpLastWorldMips() {
        if (!g_renderBackend) return;
        for (int face = 0; face < 6; ++face) {
            if (s_mipUploaded[face] || !s_mipJobs[face].valid()) continue;
            if (s_mipJobs[face].wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            const Mipmap::Image mip = s_mipJobs[face].get();   // the future is spent now
            const TextureHandle tex = s_lastWorldTex[face];
            s_mipUploaded[face] = true;
            if (tex == INVALID_TEXTURE || mip.width <= 0) continue;
            g_renderBackend->UploadTextureMipLevel(tex, 1, mip.width, mip.height, mip.pixels.data());
            g_renderBackend->SetTextureFilter(tex, TextureFilter::LinearMipmapLinear, TextureFilter::Linear);
            return;   // one a frame: ~10 MB, well inside a vsync frame
        }
    }

    void PanoramaRenderer::DiscardAdoptedFaces() {
        for (int i = 0; i < 6; ++i) {
            TextureHandle& t = s_adoptedFaces[i];
            if (t != INVALID_TEXTURE && g_renderBackend) g_renderBackend->DeferredDestroyTexture(t);
            if (s_lastWorldTex[i] == t) s_lastWorldTex[i] = INVALID_TEXTURE;
            t = INVALID_TEXTURE;
        }
        s_adoptedSize = 0;
    }

    std::string PanoramaRenderer::s_lastWorldId;
    float       PanoramaRenderer::s_lastWorldBaseYaw = 0.0f;
    float       PanoramaRenderer::s_lastWorldLeaveYaw = 0.0f;
    float       PanoramaRenderer::s_lastWorldLeavePitch = 0.0f;
    bool        PanoramaRenderer::s_lastWorldMetaRead = false;

    namespace {
        void ReadLastWorldMeta(std::string& worldId, float& baseYaw, float& leaveYaw, float& leavePitch) {
            worldId.clear();
            baseYaw = leaveYaw = leavePitch = 0.0f;
            std::ifstream in(PanoramaRenderer::LastWorldDir() + "meta.json");
            if (!in) return;
            try {
                const nlohmann::json j = nlohmann::json::parse(in);
                worldId    = j.value("world", std::string());
                baseYaw    = j.value("baseYaw", 0.0f);
                leaveYaw   = j.value("leaveYaw", baseYaw);
                leavePitch = j.value("leavePitch", 0.0f);
            } catch (...) {
                worldId.clear();
            }
        }
    } // namespace

    void PanoramaRenderer::EnsureLastWorldMeta() {
        if (s_lastWorldMetaRead) return;
        ReadLastWorldMeta(s_lastWorldId, s_lastWorldBaseYaw, s_lastWorldLeaveYaw, s_lastWorldLeavePitch);
        s_lastWorldMetaRead = true;
    }

    std::string PanoramaRenderer::LastWorldId()   { EnsureLastWorldMeta(); return s_lastWorldId; }
    float PanoramaRenderer::LastWorldBaseYaw()    { EnsureLastWorldMeta(); return s_lastWorldBaseYaw; }
    float PanoramaRenderer::LastWorldLeaveYaw()   { EnsureLastWorldMeta(); return s_lastWorldLeaveYaw; }
    float PanoramaRenderer::LastWorldLeavePitch() { EnsureLastWorldMeta(); return s_lastWorldLeavePitch; }

    void PanoramaRenderer::SaveLastWorldAsync(std::shared_ptr<LastWorldFaces> faces) {
        if (!faces || faces->size <= 0) return;
        s_memoryFaces = faces;
        // The identity is known now; the files follow.
        s_lastWorldId         = faces->worldId;
        s_lastWorldBaseYaw    = faces->baseYaw;
        s_lastWorldLeaveYaw   = faces->leaveYaw;
        s_lastWorldLeavePitch = faces->leavePitch;
        s_lastWorldMetaRead   = true;
        // A previous leave's writer may still be going (it runs at low
        // priority and the files are large). It is joined INSIDE the new
        // thread — never here, on the main thread, where it would stall
        // the transition — so the two never write the same files at once.
        std::thread previous = std::move(s_saveThread);
        s_saveThread = std::thread([faces, prev = std::move(previous)]() mutable {
            if (prev.joinable()) prev.join();
            // Low priority: six large PNG encodes start right as the title
            // comes up, and at normal priority they contend with the
            // render thread through the transition's last frames. Utility,
            // not background: macOS throttles background work hard enough
            // that a second leave could wait on the first's files for ages.
#if defined(__APPLE__)
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            PROFILE_THREAD("PanoramaSave");
            SaveLastWorld(*faces);
        });
    }

    void PanoramaRenderer::FinishPendingSaves() {
        if (s_saveThread.joinable()) s_saveThread.join();
        for (auto& job : s_mipJobs) if (job.valid()) job.wait();
    }

    bool PanoramaRenderer::SaveLastWorld(const LastWorldFaces& faces) {
        const auto& facesRgba = faces.rgba;
        const int size = faces.size;
        if (size <= 0) return false;
        // Speed over size: level 8 (the default) is most of those seconds,
        // and the files are read once at a title screen.
        stbi_write_png_compression_level = 1;
        const std::filesystem::path dir = LastWorldDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            Log::Warning("PanoramaRenderer: cannot create %s: %s", dir.string().c_str(), ec.message().c_str());
            return false;
        }
        const size_t bytes = static_cast<size_t>(size) * static_cast<size_t>(size) * 4u;
        for (int i = 0; i < 6; ++i) {
            const auto& face = facesRgba[static_cast<size_t>(i)];
            if (face.size() != bytes) {
                Log::Warning("PanoramaRenderer: face %d is not %dx%d — last-world panorama not saved", i, size, size);
                return false;
            }
            const std::string path = (dir / ("panorama_" + std::to_string(i) + ".png")).string();
            PROFILE_ZONE_N("Panorama.WritePNG");
            if (!stbi_write_png(path.c_str(), size, size, 4, face.data(), size * 4)) {
                Log::Warning("PanoramaRenderer: could not write %s", path.c_str());
                return false;
            }
        }
        {
            nlohmann::json j;
            j["world"]      = faces.worldId;
            j["baseYaw"]    = faces.baseYaw;
            j["leaveYaw"]   = faces.leaveYaw;
            j["leavePitch"] = faces.leavePitch;
            std::ofstream out((dir / "meta.json").string());
            if (out) out << j.dump(2);
        }
        Log::Info("PanoramaRenderer: saved the last-world panorama (%dx%d) to %s", size, size, dir.string().c_str());
        return true;
    }

    bool PanoramaRenderer::Initialize(const std::string& setSlug) {
        if (m_initialized) {
            // Back at the title from a world: the captured set has just been
            // rewritten, a random pick should roll again, and the option may
            // have changed in-game. Only an unchanged shipped set is kept.
            if (setSlug == kLastWorldSet || setSlug == kRandomSet || setSlug != m_currentSet) {
                LoadSet(setSlug);
            }
            return true;
        }
        if (!g_renderBackend) return false;

        // Try SPIR-V (Vulkan rewrites to panorama_vk.*.spv), then fall back
        // to GLSL source (OpenGL) — same pattern as BlockBreakOverlay. Kept
        // across Shutdown, so only the first Initialize builds it.
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                "shaders/panorama.vert", "shaders/panorama.frag");
        }
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShader(vertexShaderSource, fragmentShaderSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("PanoramaRenderer: shader creation failed");
            return false;
        }

        // Inward-facing unit cube around the origin. One face per draw so
        // each can bind its own 2D texture (equivalent to MC's cubemap
        // sampler, no new backend capability needed).
        //
        // Face order matches the panorama_N convention:
        //   0 = front (-Z when yaw 0), 1 = right (+X), 2 = back (+Z),
        //   3 = left (-X), 4 = up, 5 = down.
        struct V { float x, y, z; float u, v; uint8_t r, g, b, a; };
        static_assert(sizeof(V) == 24, "vertex stride must match block layout");
        const uint8_t W = 255;
        std::vector<V> verts;
        std::vector<uint32_t> idx;
        verts.reserve(24);
        idx.reserve(36);

        // Adds one face as (top-left, top-right, bottom-right, bottom-left)
        // as seen from INSIDE the cube, with upright UVs.
        auto addFace = [&](glm::vec3 tl, glm::vec3 tr, glm::vec3 br, glm::vec3 bl) {
            uint32_t base = static_cast<uint32_t>(verts.size());
            verts.push_back({tl.x, tl.y, tl.z, 0.0f, 0.0f, W, W, W, W});
            verts.push_back({tr.x, tr.y, tr.z, 1.0f, 0.0f, W, W, W, W});
            verts.push_back({br.x, br.y, br.z, 1.0f, 1.0f, W, W, W, W});
            verts.push_back({bl.x, bl.y, bl.z, 0.0f, 1.0f, W, W, W, W});
            idx.insert(idx.end(), {base + 0, base + 1, base + 2,
                                   base + 0, base + 2, base + 3});
        };

        const float s = 1.0f;
        // 0: front (-Z)
        addFace({-s,  s, -s}, { s,  s, -s}, { s, -s, -s}, {-s, -s, -s});
        // 1: right (+X)
        addFace({ s,  s, -s}, { s,  s,  s}, { s, -s,  s}, { s, -s, -s});
        // 2: back (+Z)
        addFace({ s,  s,  s}, {-s,  s,  s}, {-s, -s,  s}, { s, -s,  s});
        // 3: left (-X)
        addFace({-s,  s,  s}, {-s,  s, -s}, {-s, -s, -s}, {-s, -s,  s});
        // 4: up (+Y) — top edge continues from the front face
        addFace({-s,  s,  s}, { s,  s,  s}, { s,  s, -s}, {-s,  s, -s});
        // 5: down (-Y)
        addFace({-s, -s, -s}, { s, -s, -s}, { s, -s,  s}, {-s, -s,  s});

        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
            verts.size() * sizeof(V), verts.data());
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
            idx.size() * sizeof(uint32_t), idx.data());
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        m_initialized = true;
        LoadSet(setSlug);
        return true;
    }

    void PanoramaRenderer::DestroyFaceTextures() {
        // Deferred: the faces were drawn last frame, and the immediate
        // destroy drains the device per texture — six drains on the join's
        // hand-over frame was the hitch at the landing. The backend frees
        // them once the frame that drew them has passed its fence.
        for (auto& f : m_faces) {
            if (f == INVALID_TEXTURE) continue;
            for (TextureHandle& t : s_lastWorldTex) if (t == f) t = INVALID_TEXTURE;   // no mip upload into a dead texture
            g_renderBackend->DeferredDestroyTexture(f);
            f = INVALID_TEXTURE;
        }
        if (m_overlay != INVALID_TEXTURE) { g_renderBackend->DeferredDestroyTexture(m_overlay); m_overlay = INVALID_TEXTURE; }
    }

    // Loads the six faces (+ optional overlay) of one set. All six must be
    // present and not 1×1 stubs; on failure the previous textures are gone
    // and m_texturesValid is false (gradient fallback).
    bool PanoramaRenderer::TryLoadSet(const std::string& slug) {
        DestroyFaceTextures();
        m_easeActive = m_easeDone = false;   // a new set drifts freely
        bool allValid = true;
        // The set just captured, already on the GPU (built tile by tile
        // during the capture): adopt the textures outright.
        if (slug == kLastWorldSet && s_adoptedSize > 0) {
            bool all = true;
            for (int i = 0; i < 6; ++i) if (s_adoptedFaces[i] == INVALID_TEXTURE) { all = false; break; }
            if (all) {
                for (int i = 0; i < 6; ++i) { m_faces[i] = s_adoptedFaces[i]; s_adoptedFaces[i] = INVALID_TEXTURE; }
                Log::Info("PanoramaRenderer: last-world set adopted from the capture (%dx%d faces)", s_adoptedSize, s_adoptedSize);
                s_adoptedSize   = 0;
                s_memoryFaces.reset();   // the writer thread holds its own reference
                m_overlay       = INVALID_TEXTURE;   // a capture has no overlay
                m_texturesValid = true;
                m_currentSet    = slug;
                return true;
            }
            DiscardAdoptedFaces();
        }
        // Else the set just captured, still in memory: textures straight
        // from it, and the copy goes — the PNGs (being written in the
        // background) serve any later load.
        if (slug == kLastWorldSet && s_memoryFaces && s_memoryFaces->size > 0) {
            const int size = s_memoryFaces->size;
            for (int i = 0; i < 6; ++i) {
                const auto& px = s_memoryFaces->rgba[static_cast<size_t>(i)];
                if (px.size() != static_cast<size_t>(size) * static_cast<size_t>(size) * 4u) { allValid = false; break; }
                m_faces[i] = g_renderBackend->CreateTexture2D(size, size, TextureFormat::RGBA8, px.data());
                if (m_faces[i] == INVALID_TEXTURE) { allValid = false; break; }
                g_renderBackend->SetTextureFilter(m_faces[i], TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(m_faces[i], TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            s_memoryFaces.reset();
            if (allValid) {
                m_overlay       = INVALID_TEXTURE;   // a capture has no overlay
                m_texturesValid = true;
                m_currentSet    = slug;
                return true;
            }
            DestroyFaceTextures();
            allValid = true;   // fall through to the files
        }
        for (int i = 0; i < 6; ++i) {
            bool stub = false;
            m_faces[i] = LoadFaceTexture(FacePath(slug, "panorama_" + std::to_string(i) + ".png"), stub);
            if (m_faces[i] == INVALID_TEXTURE) allValid = false;
        }
        if (!allValid) {
            DestroyFaceTextures();
            m_texturesValid = false;
            return false;
        }
        bool stub = false;
        m_overlay = LoadFaceTexture(FacePath(slug, "panorama_overlay.png"), stub);
        m_texturesValid = true;
        m_currentSet = slug;
        return true;
    }

    void PanoramaRenderer::LoadSet(const std::string& slug) {
        if (!m_initialized || !g_renderBackend) return;
        std::string want = slug.empty() ? kDefaultSet : slug;
        if (want == kRandomSet) {
            // Random draws from the shipped sets only — the captured one is
            // a deliberate choice, and may not exist yet.
            auto sets = AvailableSets();
            sets.erase(std::remove_if(sets.begin(), sets.end(),
                                      [](const SetInfo& s) { return std::string_view(s.slug) == kLastWorldSet; }),
                       sets.end());
            if (!sets.empty()) {
                static std::mt19937 rng{std::random_device{}()};
                std::uniform_int_distribution<size_t> pick(0, sets.size() - 1);
                want = sets[pick(rng)].slug;
                Log::Info("PanoramaRenderer: random panorama -> '%s'", want.c_str());
            } else {
                want = kDefaultSet;
            }
        }
        if (TryLoadSet(want)) { ApplyHandoff(want); return; }
        if (want != kDefaultSet && TryLoadSet(kDefaultSet)) {
            Log::Warning("PanoramaRenderer: set '%s' missing — fell back to '%s'",
                         want.c_str(), kDefaultSet);
            ApplyHandoff(kDefaultSet);
            return;
        }
        ApplyHandoff("");
        Log::Info("PanoramaRenderer: panorama textures missing — using gradient fallback "
                  "(expected assets/textures/gui/title/background/%s/panorama_0..5.png)",
                  want.c_str());
    }

    void PanoramaRenderer::Shutdown() {
        // Does NOT wait for a pending save: the join transition shuts the
        // renderer down on the hand-over frame while the PNGs may still be
        // writing. The process exit waits (FinishPendingSaves there).
        if (!g_renderBackend) return;
        DestroyFaceTextures();
        // Deferred too: the cube was drawn last frame. The shader stays —
        // destroying it drains the device and drops its pipelines, and the
        // next Initialize (back at the title) reuses it.
        if (m_mesh   != INVALID_MESH)   { g_renderBackend->DeferredDestroyMesh(m_mesh);   m_mesh = INVALID_MESH; }
        if (m_vb     != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(m_vb);   m_vb = INVALID_BUFFER; }
        if (m_ib     != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(m_ib);   m_ib = INVALID_BUFFER; }
        m_initialized = false;
        m_texturesValid = false;
    }

    void PanoramaRenderer::Render(int fbWidth, int fbHeight, float deltaSeconds, float speed) {
        if (!g_renderBackend || fbWidth <= 0 || fbHeight <= 0) return;

        // MC PanoramaRenderer.render: spin += realtimeTicks * speed * 0.1 per
        // frame → 0.1°/tick = 2°/second at speed 1.0.
        // The drift — off while an EaseTo steers, and after it has landed.
        if (!m_easeActive && !m_easeDone) {
            m_spin += deltaSeconds * 20.0f * 0.1f * speed;
            while (m_spin >= 360.0f) m_spin -= 360.0f;
        }

        if (!m_texturesValid || m_mesh == INVALID_MESH) return;

        PipelineState state;
        state.depthTestEnabled  = false;
        state.depthWriteEnabled = false;
        state.blendEnabled      = false;
        state.cullMode          = CullMode::None;
        state.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);

        // MC CubeMap.render: 85° perspective, fixed 10° downward pitch,
        // yaw = -spin.
        // The title's own framing: 85° and a 10° look-down. Straight after
        // a world, the captured set starts at the player's pitch and field
        // of view and eases to these over four seconds (SetHandoff).
        float pitchDeg = 10.0f;
        float fovDeg   = 85.0f;
        if (m_easeActive || m_easeDone) {
            // EaseTo: steer spin, pitch and lens together, then hold.
            const float t = m_easeDone ? 1.0f : std::clamp(
                std::chrono::duration<float>(std::chrono::steady_clock::now() - m_easeStart).count() / m_easeSeconds,
                0.0f, 1.0f);
            const float e = t * t * (3.0f - 2.0f * t);
            m_spin   = glm::mix(m_spinFrom, m_spinTo, e);
            pitchDeg = glm::mix(m_easePitchFrom, m_easePitchTo, e);
            fovDeg   = glm::mix(m_easeFovFrom,   m_easeFovTo,   e);
            if (t >= 1.0f && m_easeActive) { m_easeActive = false; m_easeDone = true; }
        } else if (m_handoffActive) {
            const float t = std::clamp(
                std::chrono::duration<float>(std::chrono::steady_clock::now() - m_handoffStart).count() / 4.0f,
                0.0f, 1.0f);
            const float e = t * t * (3.0f - 2.0f * t);   // smoothstep
            pitchDeg = glm::mix(m_pitchFrom, 10.0f, e);
            fovDeg   = glm::mix(m_fovFrom,   85.0f, e);
            if (t >= 1.0f) m_handoffActive = false;
        }
        m_lastPitch = pitchDeg;
        m_lastFov   = fovDeg;
        const float aspect = static_cast<float>(fbWidth) / static_cast<float>(fbHeight);
        glm::mat4 proj = glm::perspective(glm::radians(fovDeg), aspect, 0.05f, 10.0f);
        glm::mat4 view(1.0f);
        // rotX(+p) looks DOWN by p (MC's pitch sign); rotY(-spin) turns the
        // view LEFT as spin grows.
        view = glm::rotate(view, glm::radians(pitchDeg),  glm::vec3(1, 0, 0));
        view = glm::rotate(view, glm::radians(-m_spin), glm::vec3(0, 1, 0));
        glm::mat4 mvp = proj * view;
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformInt(m_shader, "uTexture", 0);

        // One draw per face, 6 indices each, so each face binds its texture.
        // DrawIndexed's offset is in index ELEMENTS (GLBackend multiplies by
        // sizeof(uint32_t) itself).
        for (uint32_t face = 0; face < 6; ++face) {
            g_renderBackend->BindTexture(m_faces[face], 0);
            g_renderBackend->DrawIndexed(m_mesh, 6, face * 6);
        }
        g_renderBackend->UnbindMesh();

        // Restore defaults for the GUI pass.
        PipelineState def;
        def.depthTestEnabled  = true;
        def.depthWriteEnabled = true;
        def.blendEnabled      = false;
        def.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(def);
    }

    void PanoramaRenderer::RenderOverlay(GuiGraphics& g, int guiWidth, int guiHeight, float alpha) {
        if (!m_texturesValid) {
            // Gradient fallback: deep night-sky wash so logo/buttons read well.
            g.FillGradient(0, 0, guiWidth, guiHeight, 0xFF0B1026, 0xFF05070F);
            return;
        }
        if (m_overlay != INVALID_TEXTURE) {
            g.Blit(m_overlay, 0, 0, guiWidth, guiHeight, 0.0f, 0.0f, 1.0f, 1.0f,
                   ApplyAlpha(0xFFFFFFFF, alpha));
        }
    }

} // namespace Render
