// File: src/client/renderer/environment/SkyRenderer.cpp
//
// Geometry and constants are verbatim from the vendored MC decompile
// (minecraft_code/.../client/renderer/SkyRenderer.java):
//   SKY_DISC_RADIUS 512, disc fan of 10 verts at y=±16, sunrise fan of 18
//   verts (center (0,100,0), ring r=120 with z=-cos*40), sun 30×(0,100,0),
//   moon 20×(0,100,0), 1500 star attempts from RandomSource.create(10842L).
// Blend modes from RenderPipelines.java: SKY opaque, SUNRISE_SUNSET
// translucent, CELESTIAL/STARS "overlay" additive (SRC_ALPHA, ONE).
// Only the sky discs are fogged (sky.fsh: apply_fog with FogSkyEnd).
#include "SkyRenderer.hpp"
#include "platform/GameDirectory.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "EnvironmentState.hpp"
#include "JavaRandom.hpp"
#include "../backend/RenderBackend.hpp"
#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif
#include "common/core/Log.hpp"
#include "common/world/biome/Biomes.hpp"
#include "stb_image.h"
#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    SkyRenderer g_skyRenderer;

    // GL shader. VK uses shaders/sky_vk.{vert,frag} (portal pipeline layout:
    // push constants for uMVP/uColor, CommonUBO for uFogColor/uFogEnv).
    const char* SkyRenderer::vertexShaderSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;
out float vSph;
out float vCyl;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    // MC sky.vsh: fog distances from the RAW buffer position (the sky is
    // camera-centered, so model-space distance == camera distance).
    vSph = length(aPos);
    vCyl = max(length(aPos.xz), abs(aPos.y));
}
)";

    const char* SkyRenderer::fragmentShaderSource = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in float vSph;
in float vCyl;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec4 uColor;
uniform vec4 uFogColor;
uniform vec4 uFogEnv;   // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 color = texture(uTexture, vUV) * vColor * uColor;
    if (color.a == 0.0) discard;   // MC position_tex.fsh texel discard
    float fogValue = max(linearFog(vSph, uFogEnv.x, uFogEnv.y),
                         linearFog(vCyl, uFogEnv.z, uFogEnv.w));
    FragColor = vec4(mix(color.rgb, uFogColor.rgb, fogValue * uFogColor.a), color.a);
}
)";

    namespace {

        constexpr float kSkyDiscRadius = 512.0f;
        constexpr float kSunSize = 30.0f;
        constexpr float kMoonSize = 20.0f;
        constexpr float kCelestialHeight = 100.0f;
        constexpr int kStarAttempts = 1500;
        constexpr int64_t kStarSeed = 10842;

        TextureHandle LoadTextureFile(const std::string& full) {
            if (!std::filesystem::exists(full)) {
                Log::Warning("SkyRenderer: missing texture %s", full.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) return INVALID_TEXTURE;
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                // Vanilla binds sun/moon with default (nearest) sampling.
                g_renderBackend->SetTextureFilter(t, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return t;
        }
        TextureHandle LoadEnvTexture(const std::string& relPath) {
            return LoadTextureFile(PlatformMain::GetAssetPath(relPath));
        }

        ShaderHandle CreateSkyShader(const char* vertSrc, const char* fragSrc) {
            // Vulkan needs the UBO-aware (portal) pipeline layout for
            // uFogColor/uFogEnv — same backend-cast pattern as PortalRenderer.
            if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
                auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
                return vk->CreateShaderFromFilesPortal("shaders/sky.vert", "shaders/sky.frag");
#else
                return INVALID_SHADER;
#endif
            }
            ShaderHandle s = g_renderBackend->CreateShaderFromFiles("shaders/sky.vert", "shaders/sky.frag");
            if (s == INVALID_SHADER) {
                s = g_renderBackend->CreateShader(vertSrc, fragSrc);
            }
            return s;
        }

        const glm::vec4 kFogOff{1e9f, 1e9f, 1e9f, 1e9f};

        // MC buildEndSky vertex color -14145496 = 0xFF282828.
        const glm::vec3 kEndSkyTint{0x28 / 255.0f, 0x28 / 255.0f, 0x28 / 255.0f};

        constexpr const char* kSkyboxDir = "assets/textures/environment/skyboxes/";
        constexpr const char* kPanoramaDir = "assets/textures/gui/title/background/";

        // The three roots a set may live in, in lookup order (see
        // SkyboxSource). Each is an absolute directory, or empty when it
        // does not exist. A folder name is forbidden to contain a path
        // separator so an id from worlds.json can never escape a root.
        struct SkyboxRoot { SkyboxSource source; std::string dir; };
        std::vector<SkyboxRoot> SkyboxRoots() {
            std::vector<SkyboxRoot> roots;
            const std::string user = UserSkyboxDirectory();
            if (!user.empty()) roots.push_back({SkyboxSource::User, user});
            roots.push_back({SkyboxSource::Builtin, PlatformMain::GetAssetPath(kSkyboxDir)});
            roots.push_back({SkyboxSource::Panorama, PlatformMain::GetAssetPath(kPanoramaDir)});
            return roots;
        }
        // Extensions tried for a face, in this order. All are formats
        // stb_image decodes; PNG first because that is what the shipped
        // sets and the README say.
        constexpr const char* kFaceExtensions[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga" };

        bool ValidSkyboxId(const std::string& id) {
            return !id.empty() && id.find('/') == std::string::npos &&
                   id.find('\\') == std::string::npos && id != "." && id != "..";
        }

        // The folder inside a set that holds OptiFine's sky<n>.properties
        // for one world (with a trailing separator), or empty. OptiFine
        // reads optifine/sky/world<N>: world0 is the overworld, world1 the
        // End (the Nether draws no sky). A pack folder keeps them under
        // assets/minecraft; a bare folder may hold the overworld's directly.
        std::string FindOptiFineSkyDir(const std::string& setDir, const char* world = "world0") {
            const std::string w = world;
            std::vector<std::string> candidates = {
                "assets/minecraft/optifine/sky/" + w + "/",
                "assets/minecraft/mcpatcher/sky/" + w + "/",   // OptiFine's older location, still read
                "optifine/sky/" + w + "/",
                "sky/" + w + "/",
            };
            if (w == "world0") candidates.emplace_back();   // a bare folder holds the overworld's
            std::error_code ec;
            for (const std::string& sub : candidates) {
                const std::string dir = setDir + sub;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (!entry.is_regular_file(ec)) continue;
                    const std::string name = entry.path().filename().string();
                    if (name.size() > 14 && name.rfind("sky", 0) == 0 &&
                        name.compare(name.size() - 11, 11, ".properties") == 0) {
                        return dir;
                    }
                }
            }
            return {};
        }
        bool HasOptiFineSky(const std::string& setDir) {
            return !FindOptiFineSkyDir(setDir, "world0").empty() ||
                   !FindOptiFineSkyDir(setDir, "world1").empty();
        }
        // OptiFine reads its sky from the enabled resource packs: the
        // highest one carrying optifine/sky wins. Empty when none does.
        std::string ResourcePackSkyRoot() {
            for (const std::string& root : Resources::EnabledPackRoots()) {
                if (HasOptiFineSky(root)) return root;
            }
            return {};
        }

        // sky<n>.properties files of a folder, in layer order (by n).
        std::vector<std::string> OptiFineLayerFiles(const std::string& skyDir) {
            std::vector<std::pair<int, std::string>> found;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(skyDir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const std::string name = entry.path().filename().string();
                if (name.rfind("sky", 0) != 0 || name.size() <= 14 ||
                    name.compare(name.size() - 11, 11, ".properties") != 0) continue;
                const std::string digits = name.substr(3, name.size() - 14);
                if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
                found.emplace_back(std::atoi(digits.c_str()), entry.path().string());
            }
            std::sort(found.begin(), found.end());
            std::vector<std::string> out;
            for (auto& [n, path] : found) out.push_back(path);
            return out;
        }

        // The minimal Java .properties reader OptiFine's files need:
        // key=value lines, # comments, surrounding whitespace dropped.
        std::vector<std::pair<std::string, std::string>> ReadProperties(const std::string& path) {
            std::vector<std::pair<std::string, std::string>> out;
            std::ifstream in(path);
            std::string line;
            auto trim = [](std::string s) {
                const auto a = s.find_first_not_of(" \t\r\n");
                if (a == std::string::npos) return std::string();
                const auto b = s.find_last_not_of(" \t\r\n");
                return s.substr(a, b - a + 1);
            };
            while (std::getline(in, line)) {
                line = trim(line);
                if (line.empty() || line[0] == '#' || line[0] == '!') continue;
                const auto eq = line.find_first_of("=:");
                if (eq == std::string::npos) continue;
                out.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
            }
            return out;
        }

        // OptiFine's "hh:mm" → ticks: 6:00 is tick 0 (CommonUtils.toTickTime).
        int OptiFineTimeToTicks(const std::string& text) {
            const auto colon = text.find(':');
            if (colon == std::string::npos) return -1;
            const int h = std::atoi(text.substr(0, colon).c_str());
            const int m = std::atoi(text.substr(colon + 1).c_str());
            if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
            int hh = h - 6;
            if (hh < 0) hh += 24;
            return hh * 1000 + static_cast<int>(m / 60.0f * 1000.0f);
        }
        int NormalizeTick(int t) {
            int r = t % 24000;
            return r < 0 ? r + 24000 : r;
        }
        bool InTimeInterval(int now, int start, int end) {
            if (now < 0 || now >= 24000) return false;
            return start <= end ? (now >= start && now <= end) : (now >= start || now <= end);
        }
        int CyclicDistance(int start, int end) { return (end - start + 24000) % 24000; }

        // Resolve a layer's `source` against the pack, OptiFine's way:
        //   ./name      next to the properties file
        //   ~/path      assets/minecraft/optifine/path
        //   assets/...  from the pack root
        //   name        next to the properties file
        //   ns:path     assets/<ns>/<path> — a resource location, the way
        //               packs that also target the Fabric sky mods write it
        //               (skybox:stars.png, minecraft:textures/...)
        std::string ResolveOptiFineSource(const std::string& source, const std::string& setDir,
                                          const std::string& skyDir, const std::string& fallbackName) {
            std::string s = source.empty() ? fallbackName : source;
            if (s.rfind("./", 0) == 0) return skyDir + s.substr(2);
            if (s.rfind("~/", 0) == 0) return setDir + "assets/minecraft/optifine/" + s.substr(2);
            if (s.rfind("assets/", 0) == 0) return setDir + s;
            const size_t colon = s.find(':');
            if (colon != std::string::npos && colon > 0 && s.find('/') > colon) {
                return setDir + "assets/" + s.substr(0, colon) + "/" + s.substr(colon + 1);
            }
            if (s.find('/') == std::string::npos) return skyDir + s;
            return setDir + s;
        }

        // Space-separated words of a properties value.
        std::vector<std::string> SplitWords(const std::string& text) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : text) {
                if (c == ' ' || c == '\t' || c == ',') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
                else cur += c;
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        }
        bool ParseInt(const std::string& t, int& out) {
            if (t.empty()) return false;
            char* end = nullptr;
            const long v = std::strtol(t.c_str(), &end, 10);
            if (end == t.c_str() || *end != '\0') return false;
            out = static_cast<int>(v);
            return true;
        }
        // OptiFine range list: "0-64 100-128 200", open ends "64-" / "-64"
        // written with digits on one side only; "-64-0" is a negative low
        // end. Tokens that do not parse are dropped.
        std::vector<SkyIntRange> ParseIntRanges(const std::string& text) {
            std::vector<SkyIntRange> out;
            for (const std::string& tok : SplitWords(text)) {
                size_t dash = std::string::npos;
                for (size_t i = 1; i < tok.size(); ++i) {
                    if (tok[i] == '-' && tok[i - 1] != '-') { dash = i; break; }
                }
                SkyIntRange r;
                int v = 0;
                if (dash == std::string::npos) {
                    if (!ParseInt(tok, v)) continue;
                    r.lo = r.hi = v;
                } else {
                    const std::string a = tok.substr(0, dash), b = tok.substr(dash + 1);
                    if (!a.empty()) { if (!ParseInt(a, v)) continue; r.lo = v; }
                    if (!b.empty()) { if (!ParseInt(b, v)) continue; r.hi = v; }
                    if (r.lo > r.hi) std::swap(r.lo, r.hi);
                }
                out.push_back(r);
            }
            return out;
        }
        bool InRanges(const std::vector<SkyIntRange>& ranges, int v) {
            for (const SkyIntRange& r : ranges) if (v >= r.lo && v <= r.hi) return true;
            return false;
        }
        // "biomes=minecraft:plains forest !swamp": names with or without
        // the namespace; a leading '!' on the first name turns the list
        // into an exclusion (OptiFine 1.19+). Unknown names are dropped
        // with a warning rather than silently matching the fallback biome.
        void ParseBiomeList(const std::string& text, std::vector<uint16_t>& out, bool& negated) {
            negated = false;
            for (std::string name : SplitWords(text)) {
                if (!name.empty() && name[0] == '!') { negated = true; name.erase(0, 1); }
                if (name.rfind("minecraft:", 0) == 0) name.erase(0, 10);
                if (name.empty()) continue;
                const Game::BiomeId id = Game::BiomeRegistry::FromName(name);
                if (Game::BiomeRegistry::Get(id).name != name) {
                    Log::Warning("SkyRenderer: OptiFine sky: unknown biome '%s' ignored", name.c_str());
                    continue;
                }
                out.push_back(id);
            }
        }
        // Minecraft samples a resource-pack texture with nearest filtering
        // unless its <file>.mcmeta says {"texture": {"blur": true}}.
        bool TextureBlur(const std::string& texPath) {
            std::ifstream in(texPath + ".mcmeta");
            if (!in) return false;
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false, true);
            if (j.is_discarded() || !j.is_object()) return false;
            const auto t = j.find("texture");
            if (t == j.end() || !t->is_object()) return false;
            return t->value("blur", false);
        }

        // Where the six faces sit in OptiFine's 3×2 texture (cells numbered
        // row-major from the top left), per its labelled template:
        //   0 bottom | 1 top   | 2 south
        //   3 west   | 4 north | 5 east
        // and which panorama_N cube face each one is (0 front −Z north,
        // 1 right +X east, 2 back +Z south, 3 left −X west, 4 up, 5 down).
        // Every cell copies straight across, top and bottom included: the
        // template's "N" marks (bottom edge of Top, top edge of Bottom)
        // are exactly where the −Z edge of our up/down faces is. Derived
        // by transforming Skyboxify's SkyPart face matrices (after the
        // sky pass's −90° Y pre-rotation) and matching our cube face by
        // face; a first version took the cells from that enum's NAMES,
        // which are one step off from what its matrices do, and turned
        // the top and bottom — the sides sat 90° round and the top face
        // did not meet them, which read as a line across the sky.
        constexpr int kOptiFineFaceCells[6] = {
            4,   // panorama_0 front  −Z ← north
            5,   // panorama_1 right  +X ← east
            2,   // panorama_2 back   +Z ← south
            3,   // panorama_3 left   −X ← west
            1,   // panorama_4 up        ← top
            0,   // panorama_5 down      ← bottom
        };

        // Resolve a skybox set id to the absolute directory of the set
        // (with a trailing separator), or empty.
        std::string ResolveSkyboxDir(const std::string& id) {
            if (!ValidSkyboxId(id)) return {};
            for (const SkyboxRoot& root : SkyboxRoots()) {
                const std::string dir = (std::filesystem::path(root.dir) / id).string() + "/";
                if (!SkyboxFacePath(dir, 0).empty()) return dir;
                if (HasOptiFineSky(dir)) return dir;
            }
            return {};
        }

        // Loads one skybox face from an absolute path; optionally
        // accumulates the average color of the texture's middle row (the
        // horizon band on side faces) so the fog color can match the skybox.
        TextureHandle LoadSkyboxFace(const std::string& full,
                                     glm::vec3* horizonAccum, int* horizonSamples) {
            if (full.empty() || !std::filesystem::exists(full)) return INVALID_TEXTURE;
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) return INVALID_TEXTURE;
            if (horizonAccum && w > 0 && h > 0) {
                const unsigned char* row = pixels + static_cast<size_t>(h / 2) * w * 4;
                for (int x = 0; x < w; ++x) {
                    horizonAccum->r += row[x * 4 + 0] / 255.0f;
                    horizonAccum->g += row[x * 4 + 1] / 255.0f;
                    horizonAccum->b += row[x * 4 + 2] / 255.0f;
                }
                *horizonSamples += w;
            }
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return t;
        }

    } // namespace

    std::string UserSkyboxDirectory() {
        return Platform::g_gameDirectory.GetSkyboxesDirectory();
    }

    bool DescribeSkyboxSet(const std::string& dir, SkyboxInfo& info) {
        if (!SkyboxFacePath(dir, 0).empty()) {
            info.kind = SkyboxKind::SixFace;
            return true;
        }
        // An OptiFine pack: the preview comes from the first overworld
        // layer, or the first End layer when the pack only has those.
        info.optifineLayers = 0;
        info.optifineTexture.clear();
        for (const char* world : {"world0", "world1"}) {
            const std::string skyDir = FindOptiFineSkyDir(dir, world);
            if (skyDir.empty()) continue;
            const std::vector<std::string> layers = OptiFineLayerFiles(skyDir);
            if (layers.empty()) continue;
            info.optifineLayers += static_cast<int>(layers.size());
            if (info.optifineTexture.empty()) {
                std::string source;
                for (const auto& [key, value] : ReadProperties(layers.front())) {
                    if (key == "source") source = value;
                }
                const std::string stem = std::filesystem::path(layers.front()).stem().string();   // sky0
                info.optifineTexture = ResolveOptiFineSource(source, dir, skyDir, stem + ".png");
            }
        }
        if (info.optifineLayers == 0) return false;
        info.kind = SkyboxKind::OptiFine;
        return true;
    }

    std::string SkyboxFacePath(const std::string& dir, int face) {
        if (dir.empty() || face < 0 || face > 5) return {};
        std::error_code ec;
        const std::string stem = dir + "panorama_" + std::to_string(face);
        for (const char* ext : kFaceExtensions) {
            const std::string path = stem + ext;
            if (std::filesystem::exists(path, ec)) return path;
        }
        return {};
    }

    std::vector<SkyboxInfo> DiscoverSkyboxes() {
        std::vector<SkyboxInfo> out;
        out.push_back({"vanilla", "Vanilla", SkyboxSource::Special, {}});
        if (std::filesystem::exists(
                PlatformMain::GetAssetPath("assets/textures/environment/end_sky.png"))) {
            out.push_back({"end", "The End", SkyboxSource::Special, {}});
        }
        // Every root in lookup order; the first folder of a given name wins,
        // matching ResolveSkyboxDir, so the picker shows what would load.
        std::vector<std::string> seen{"vanilla", "end"};
        std::error_code ec;
        for (const SkyboxRoot& root : SkyboxRoots()) {
            std::vector<SkyboxInfo> found;
            for (const auto& entry : std::filesystem::directory_iterator(root.dir, ec)) {
                if (!entry.is_directory(ec)) continue;
                const std::string name = entry.path().filename().string();
                if (!ValidSkyboxId(name)) continue;
                if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
                SkyboxInfo info;
                info.id = info.label = name;
                info.source = root.source;
                info.dir = entry.path().string() + "/";
                if (!DescribeSkyboxSet(info.dir, info)) continue;
                seen.push_back(name);
                found.push_back(std::move(info));
            }
            // Folder order is filesystem order; the picker wants a stable one.
            std::sort(found.begin(), found.end(),
                      [](const SkyboxInfo& a, const SkyboxInfo& b) { return a.id < b.id; });
            out.insert(out.end(), found.begin(), found.end());
        }
        return out;
    }

    SkyRenderer::~SkyRenderer() {
        Shutdown();
    }

    SkyRenderer::Mesh SkyRenderer::CreateMesh(const void* verts, size_t vertBytes,
                                              const uint32_t* indices, size_t indexCount) {
        Mesh m;
        m.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, vertBytes, verts);
        m.ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
                                             indexCount * sizeof(uint32_t), indices);
        m.mesh = g_renderBackend->CreateMesh(m.vb, m.ib, GetBlockVertexLayout());
        m.indexCount = static_cast<uint32_t>(indexCount);
        return m;
    }

    void SkyRenderer::DestroyMesh(Mesh& m) {
        if (m.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(m.mesh);   m.mesh = INVALID_MESH; }
        if (m.vb != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m.vb);   m.vb = INVALID_BUFFER; }
        if (m.ib != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m.ib);   m.ib = INVALID_BUFFER; }
        m.indexCount = 0;
    }

    void SkyRenderer::BuildSkyDiscs() {
        // buildSkyDisc(yy): TRIANGLE_FAN — center (0, yy, 0), then the ring
        // i = -180..180 step 45 at (signum(yy)*512*cos, yy, 512*sin).
        // signum flips the winding so both discs face the camera.
        auto buildDisc = [this](float yy) {
            std::vector<Vertex> verts;
            std::vector<uint32_t> indices;
            const float x = (yy >= 0.0f ? 1.0f : -1.0f) * kSkyDiscRadius;
            verts.push_back({0.0f, yy, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255});
            for (int i = -180; i <= 180; i += 45) {
                const float rad = glm::radians(static_cast<float>(i));
                verts.push_back({x * std::cos(rad), yy, kSkyDiscRadius * std::sin(rad),
                                 0.0f, 0.0f, 255, 255, 255, 255});
            }
            for (uint32_t i = 1; i + 1 < verts.size(); ++i) {
                indices.insert(indices.end(), {0u, i, i + 1});
            }
            return CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                              indices.data(), indices.size());
        };
        m_topDisc = buildDisc(16.0f);
        m_bottomDisc = buildDisc(-16.0f);
    }

    void SkyRenderer::BuildSunriseFan() {
        // buildSunriseFan: center (0,100,0) white alpha 1; 17 ring verts at
        // (sin*120, cos*120, -cos*40) alpha 0. ColorModulator applies the
        // actual sunrise color at draw time.
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        verts.push_back({0.0f, 100.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255});
        for (int i = 0; i <= 16; ++i) {
            const float angle = static_cast<float>(i) * glm::two_pi<float>() / 16.0f;
            const float s = std::sin(angle);
            const float c = std::cos(angle);
            verts.push_back({s * 120.0f, c * 120.0f, -c * 40.0f, 0.0f, 0.0f, 255, 255, 255, 0});
        }
        for (uint32_t i = 1; i + 1 < verts.size(); ++i) {
            indices.insert(indices.end(), {0u, i, i + 1});
        }
        m_sunriseFan = CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                                  indices.data(), indices.size());
    }

    void SkyRenderer::BuildCelestialQuads() {
        // Sun: unit quad, UV 0..1 (buildCelestialQuad).
        {
            const Vertex verts[4] = {
                {-1, 0, -1, 0.0f, 0.0f, 255, 255, 255, 255},
                { 1, 0, -1, 1.0f, 0.0f, 255, 255, 255, 255},
                { 1, 0,  1, 1.0f, 1.0f, 255, 255, 255, 255},
                {-1, 0,  1, 0.0f, 1.0f, 255, 255, 255, 255},
            };
            const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
            m_sunQuad = CreateMesh(verts, sizeof(verts), indices, 6);
        }
        // Moon: one quad per phase, UVs from the legacy 4×2 moon_phases.png
        // grid, with MC's flipped winding (u1/v1 first — buildMoonPhases).
        {
            std::vector<Vertex> verts;
            std::vector<uint32_t> indices;
            for (int phase = 0; phase < 8; ++phase) {
                const int col = phase % 4;
                const int row = phase / 4;
                const float u0 = col / 4.0f, u1 = (col + 1) / 4.0f;
                const float v0 = row / 2.0f, v1 = (row + 1) / 2.0f;
                const uint32_t base = static_cast<uint32_t>(verts.size());
                verts.push_back({-1, 0, -1, u1, v1, 255, 255, 255, 255});
                verts.push_back({ 1, 0, -1, u0, v1, 255, 255, 255, 255});
                verts.push_back({ 1, 0,  1, u0, v0, 255, 255, 255, 255});
                verts.push_back({-1, 0,  1, u1, v0, 255, 255, 255, 255});
                indices.insert(indices.end(),
                               {base, base + 1, base + 2, base, base + 2, base + 3});
            }
            m_moonQuads = CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                                     indices.data(), indices.size());
        }
    }

    void SkyRenderer::BuildStars() {
        // buildStars: 1500 attempts; RNG consumed even for rejected stars.
        JavaRandom random(kStarSeed);
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        for (int i = 0; i < kStarAttempts; ++i) {
            const float x = random.NextFloat() * 2.0f - 1.0f;
            const float y = random.NextFloat() * 2.0f - 1.0f;
            const float z = random.NextFloat() * 2.0f - 1.0f;
            const float starSize = 0.15f + random.NextFloat() * 0.1f;
            const float lengthSq = x * x + y * y + z * z;
            const float zRot = static_cast<float>(random.NextDouble() * glm::two_pi<double>());
            if (lengthSq <= 0.010000001f || lengthSq >= 1.0f) continue;

            const glm::vec3 center = glm::normalize(glm::vec3(x, y, z)) * 100.0f;
            // JOML Matrix3f.rotateTowards(-center, up): columns [left, upn, ndir].
            const glm::vec3 ndir = glm::normalize(-center);
            glm::vec3 left = glm::cross(glm::vec3(0, 1, 0), ndir);
            const float leftLen = glm::length(left);
            left = leftLen > 1e-6f ? left / leftLen : glm::vec3(1, 0, 0);
            const glm::vec3 upn = glm::cross(ndir, left);
            const glm::mat3 rt(left, upn, ndir);
            const float c = std::cos(-zRot), s = std::sin(-zRot);
            const glm::mat3 rz(glm::vec3(c, s, 0), glm::vec3(-s, c, 0), glm::vec3(0, 0, 1));
            const glm::mat3 rot = rt * rz;

            const glm::vec3 corners[4] = {
                rot * glm::vec3( starSize, -starSize, 0) + center,
                rot * glm::vec3( starSize,  starSize, 0) + center,
                rot * glm::vec3(-starSize,  starSize, 0) + center,
                rot * glm::vec3(-starSize, -starSize, 0) + center,
            };
            const uint32_t base = static_cast<uint32_t>(verts.size());
            for (const auto& p : corners) {
                verts.push_back({p.x, p.y, p.z, 0.0f, 0.0f, 255, 255, 255, 255});
            }
            indices.insert(indices.end(),
                           {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        m_stars = CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                             indices.data(), indices.size());
        Log::Info("SkyRenderer: built %zu stars (seed %lld)",
                  verts.size() / 4, static_cast<long long>(kStarSeed));
    }

    void SkyRenderer::BuildSkyboxCubes() {
        // Inward-facing unit cube, one quad per face, face order matching the
        // panorama_N convention (same geometry as PanoramaRenderer):
        //   0 = front (-Z), 1 = right (+X), 2 = back (+Z), 3 = left (-X),
        //   4 = up, 5 = down.
        // uvScale 1 for panorama sets; 16 for the End sky (MC buildEndSky
        // tiles end_sky.png 16× per face with REPEAT wrap).
        auto buildCube = [this](float uvScale) {
            std::vector<Vertex> verts;
            std::vector<uint32_t> indices;
            const float uv = uvScale;
            auto addFace = [&](glm::vec3 tl, glm::vec3 tr, glm::vec3 br, glm::vec3 bl) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                verts.push_back({tl.x, tl.y, tl.z, 0.0f, 0.0f, 255, 255, 255, 255});
                verts.push_back({tr.x, tr.y, tr.z, uv, 0.0f, 255, 255, 255, 255});
                verts.push_back({br.x, br.y, br.z, uv, uv, 255, 255, 255, 255});
                verts.push_back({bl.x, bl.y, bl.z, 0.0f, uv, 255, 255, 255, 255});
                indices.insert(indices.end(), {base, base + 1, base + 2,
                                               base, base + 2, base + 3});
            };
            const float s = 1.0f;
            addFace({-s,  s, -s}, { s,  s, -s}, { s, -s, -s}, {-s, -s, -s});  // 0 front
            addFace({ s,  s, -s}, { s,  s,  s}, { s, -s,  s}, { s, -s, -s});  // 1 right
            addFace({ s,  s,  s}, {-s,  s,  s}, {-s, -s,  s}, { s, -s,  s});  // 2 back
            addFace({-s,  s,  s}, {-s,  s, -s}, {-s, -s, -s}, {-s, -s,  s});  // 3 left
            addFace({-s,  s,  s}, { s,  s,  s}, { s,  s, -s}, {-s,  s, -s});  // 4 up
            addFace({-s, -s, -s}, { s, -s, -s}, { s, -s,  s}, {-s, -s,  s});  // 5 down
            return CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                              indices.data(), indices.size());
        };
        m_skyboxCube = buildCube(1.0f);
        m_endCube = buildCube(16.0f);
    }

    void SkyRenderer::DestroyOptiFinePack() {
        for (auto& layers : m_pack.layers) {
            for (OptiFineLayer& layer : layers) {
                for (auto& face : layer.faces) {
                    if (face != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(face); face = INVALID_TEXTURE; }
                }
            }
            layers.clear();
        }
        if (m_pack.sunTexture  != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_pack.sunTexture);
        if (m_pack.moonTexture != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_pack.moonTexture);
        m_pack = OptiFinePack{};
        m_skyboxIsOptiFine = false;
    }

    const std::string& SkyRenderer::PackCloudTexture() const {
        static const std::string kNone;
        return PackChosen() ? m_pack.cloudTexturePath : kNone;
    }

    bool SkyRenderer::LoadOptiFinePack(const std::string& id, const std::string& setDir) {
        DestroyOptiFinePack();
        const char* worlds[2] = {"world0", "world1"};
        for (int d = 0; d < 2; ++d) {
            const std::string skyDir = FindOptiFineSkyDir(setDir, worlds[d]);
            if (skyDir.empty()) continue;
            LoadOptiFineWorldLayers(setDir, skyDir, m_pack.layers[d]);
        }
        if (m_pack.layers[0].empty() && m_pack.layers[1].empty()) return false;
        m_pack.id     = id;
        m_pack.dir    = setDir;
        m_pack.loaded = true;
        ReadPackDecorations(setDir);

        // A resource pack may replace the vanilla environment textures as
        // well; a sky pack usually brings a matching sun and moon, and a
        // blank clouds.png when its clouds are painted into the sky.
        {
            const std::string env = setDir + "assets/minecraft/textures/environment/";
            std::error_code ec;
            if (std::filesystem::exists(env + "sun.png", ec))         m_pack.sunTexture  = LoadTextureFile(env + "sun.png");
            if (std::filesystem::exists(env + "moon_phases.png", ec)) m_pack.moonTexture = LoadTextureFile(env + "moon_phases.png");
            if (std::filesystem::exists(env + "clouds.png", ec))      m_pack.cloudTexturePath = env + "clouds.png";
        }

        Log::Info("SkyRenderer: OptiFine sky '%s': %zu overworld layer(s), %zu End layer(s)%s%s%s%s%s%s",
                  id.c_str(), m_pack.layers[0].size(), m_pack.layers[1].size(),
                  m_pack.showSun[0]   ? "" : ", sun hidden",
                  m_pack.showMoon[0]  ? "" : ", moon hidden",
                  m_pack.showStars[0] ? "" : ", stars hidden",
                  m_pack.sunTexture  != INVALID_TEXTURE ? ", own sun" : "",
                  m_pack.moonTexture != INVALID_TEXTURE ? ", own moon" : "",
                  m_pack.cloudTexturePath.empty() ? "" : ", own clouds");
        return true;
    }

    // The pack's Nuit (assets/nuit/sky/*.json) or FabricSkyBoxes
    // (assets/fabricskyboxes/sky/*.json) definition of the same sky, read
    // only for its sun / moon / stars switches. Both mods draw the
    // decorations PER SKYBOX ENTRY instead of the vanilla ones: a body is
    // on screen when any entry for that world draws it. In FabricSkyBoxes
    // every entry has decorations (all three on unless it says otherwise,
    // which is why packs repeat "false" in each and keep one "sunmoon"
    // entry that draws them); in Nuit only a "decorations" entry draws
    // them. An entry's "conditions.worlds" says which dimension it is for
    // (both when absent). A world with no entry keeps the vanilla bodies.
    void SkyRenderer::ReadPackDecorations(const std::string& setDir) {
        bool any[2]   = {false, false};
        bool sun[2]   = {false, false};
        bool moon[2]  = {false, false};
        bool stars[2] = {false, false};
        std::error_code ec;
        for (const char* sub : {"assets/nuit/sky/", "assets/fabricskyboxes/sky/"}) {
            const bool fsb = std::string(sub).find("fabricskyboxes") != std::string::npos;
            for (const auto& entry : std::filesystem::directory_iterator(setDir + sub, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
                std::ifstream in(entry.path());
                const nlohmann::json j = nlohmann::json::parse(in, nullptr, false, true);
                if (j.is_discarded() || !j.is_object()) continue;

                const std::string type = j.value("type", std::string());
                const nlohmann::json* deco = nullptr;
                if (type.find("decorations") != std::string::npos) {
                    deco = &j;
                } else if (const auto d = j.find("decorations"); d != j.end() && d->is_object()) {
                    deco = &*d;
                } else if (!fsb || type.empty()) {
                    continue;   // a Nuit skybox entry draws no decorations; not a skybox file
                }
                // FabricSkyBoxes entry without the object: all three on.
                const bool s = deco ? deco->value("showSun", true)   : true;
                const bool m = deco ? deco->value("showMoon", true)  : true;
                const bool t = deco ? deco->value("showStars", true) : true;

                bool worlds[2] = {true, true};
                if (const auto c = j.find("conditions"); c != j.end() && c->is_object()) {
                    if (const auto w = c->find("worlds"); w != c->end()) {
                        const nlohmann::json* list = nullptr;
                        bool excludes = false;
                        if (w->is_array()) {
                            list = &*w;
                        } else if (w->is_object()) {
                            if (const auto e = w->find("entries"); e != w->end() && e->is_array()) list = &*e;
                            excludes = w->value("excludes", false);
                        }
                        if (list) {
                            worlds[0] = worlds[1] = false;
                            for (const auto& e : *list) {
                                if (!e.is_string()) continue;
                                const std::string name = e.get<std::string>();
                                if (name == "minecraft:overworld" || name == "overworld") worlds[0] = true;
                                else if (name == "minecraft:the_end" || name == "the_end") worlds[1] = true;
                            }
                            if (excludes) { worlds[0] = !worlds[0]; worlds[1] = !worlds[1]; }
                        }
                    }
                }
                for (int d = 0; d < 2; ++d) {
                    if (!worlds[d]) continue;
                    any[d] = true;
                    sun[d]   = sun[d]   || s;
                    moon[d]  = moon[d]  || m;
                    stars[d] = stars[d] || t;
                }
            }
        }
        for (int d = 0; d < 2; ++d) {
            if (!any[d]) continue;
            m_pack.showSun[d]   = sun[d];
            m_pack.showMoon[d]  = moon[d];
            m_pack.showStars[d] = stars[d];
        }
    }

    bool SkyRenderer::LoadOptiFineWorldLayers(const std::string& setDir, const std::string& skyDir,
                                              std::vector<OptiFineLayer>& out) {
        const std::vector<std::string> files = OptiFineLayerFiles(skyDir);
        if (files.empty()) return false;

        for (const std::string& file : files) {
            OptiFineLayer layer;
            std::string source, startIn, endIn, startOut, endOut, weather;
            for (const auto& [key, value] : ReadProperties(file)) {
                if      (key == "source")       source   = value;
                else if (key == "startFadeIn")  startIn  = value;
                else if (key == "endFadeIn")    endIn    = value;
                else if (key == "startFadeOut") startOut = value;
                else if (key == "endFadeOut")   endOut   = value;
                else if (key == "blend")        layer.blend = value;
                else if (key == "rotate")       layer.rotate = (value == "true" || value == "1");
                else if (key == "speed")        layer.speed = static_cast<float>(std::atof(value.c_str()));
                else if (key == "weather")      weather = value;
                else if (key == "biomes")       ParseBiomeList(value, layer.biomes, layer.biomesNegated);
                else if (key == "heights")      layer.heights = ParseIntRanges(value);
                else if (key == "days")         layer.days = ParseIntRanges(value);
                else if (key == "daysLoop")     { int v = 0; if (ParseInt(value, v) && v > 0) layer.daysLoop = v; }
                else if (key == "transition")   layer.transitionSec = static_cast<float>(std::atof(value.c_str()));
                else if (key == "axis") {
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    if (std::sscanf(value.c_str(), "%f %f %f", &x, &y, &z) == 3 &&
                        x * x + y * y + z * z > 1e-5f) {
                        layer.axis = glm::vec3(x, y, z);
                    }
                }
            }
            // Weather: which of clear / rain / thunder the layer shows in
            // (CustomSkyLayer.weather; default clear only).
            if (!weather.empty()) {
                layer.weatherClear = layer.weatherRain = layer.weatherThunder = false;
                for (const std::string& w : SplitWords(weather)) {
                    if      (w == "clear")   layer.weatherClear   = true;
                    else if (w == "rain")    layer.weatherRain    = true;
                    else if (w == "thunder") layer.weatherThunder = true;
                }
            }

            // Fade (CommonUtils.convertOptiFineSkyProperties): all three of
            // startFadeIn/endFadeIn/endFadeOut make a fade; startFadeOut is
            // derived when missing.
            if (!startIn.empty() && !endIn.empty() && !endOut.empty()) {
                const int a = OptiFineTimeToTicks(startIn), b = OptiFineTimeToTicks(endIn),
                          d = OptiFineTimeToTicks(endOut);
                if (a >= 0 && b >= 0 && d >= 0) {
                    int c = startOut.empty() ? -1 : OptiFineTimeToTicks(startOut);
                    if (c < 0) {
                        c = d - (b - a);
                        if (a <= c && b >= c) c = d;
                    }
                    layer.fadeAlwaysOn = false;
                    layer.startFadeIn  = NormalizeTick(a);
                    layer.endFadeIn    = NormalizeTick(b);
                    layer.startFadeOut = NormalizeTick(c);
                    layer.endFadeOut   = NormalizeTick(d);
                }
            }

            // The 3×2 image, cut into the six cube faces.
            const std::string stem = std::filesystem::path(file).stem().string();
            const std::string texPath = ResolveOptiFineSource(source, setDir, skyDir, stem + ".png");
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* px = stbi_load(texPath.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!px) {
                Log::Warning("SkyRenderer: OptiFine sky layer %s: cannot read %s",
                             std::filesystem::path(file).filename().string().c_str(), texPath.c_str());
                continue;
            }
            const int cellW = w / 3, cellH = h / 2;
            if (cellW <= 0 || cellH <= 0 || cellW != cellH) {
                Log::Warning("SkyRenderer: OptiFine sky texture %s is %dx%d, not a 3x2 grid of squares",
                             texPath.c_str(), w, h);
                stbi_image_free(px);
                continue;
            }
            // MC's SimpleTexture: nearest unless the .mcmeta asks for blur;
            // no mipmaps either way.
            const TextureFilter filter = TextureBlur(texPath) ? TextureFilter::Linear : TextureFilter::Nearest;
            const int n = cellW;
            std::vector<unsigned char> face(static_cast<size_t>(n) * n * 4);
            bool ok = true;
            for (int f = 0; f < 6 && ok; ++f) {
                const int cell = kOptiFineFaceCells[f];
                const int cx0 = (cell % 3) * n, cy0 = (cell / 3) * n;
                for (int oy = 0; oy < n; ++oy) {
                    std::memcpy(face.data() + static_cast<size_t>(oy) * n * 4,
                                px + (static_cast<size_t>(cy0 + oy) * w + cx0) * 4,
                                static_cast<size_t>(n) * 4);
                }
                layer.faces[f] = g_renderBackend->CreateTexture2D(n, n, TextureFormat::RGBA8, face.data());
                if (layer.faces[f] == INVALID_TEXTURE) { ok = false; break; }
                g_renderBackend->SetTextureFilter(layer.faces[f], filter, filter);
                g_renderBackend->SetTextureWrap(layer.faces[f], TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            stbi_image_free(px);
            if (!ok) {
                for (auto& t : layer.faces) if (t != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(t); t = INVALID_TEXTURE; }
                continue;
            }
            out.push_back(std::move(layer));
        }
        return !out.empty();
    }

    void SkyRenderer::DestroySkyboxTextures() {
        // The pack (m_pack) is NOT freed here: it belongs to the player's
        // choice, not to the active dimension's sky. DestroyOptiFinePack
        // runs when the choice changes and at shutdown.
        m_skyboxIsOptiFine = false;
        for (auto& face : m_skyboxFaces) {
            if (face != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(face); face = INVALID_TEXTURE; }
        }
        if (m_endTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_endTexture);
            m_endTexture = INVALID_TEXTURE;
        }
        m_skyboxValid = false;
        m_skyboxIsEnd = false;
    }

    bool SkyRenderer::LoadSkyboxTextures(const std::string& id) {
        DestroySkyboxTextures();

        if (id == "end") {
            m_endTexture = LoadEnvTexture("assets/textures/environment/end_sky.png");
            if (m_endTexture == INVALID_TEXTURE) return false;
            // MC binds end_sky with REPEAT (16× tiling) and default sampling.
            g_renderBackend->SetTextureWrap(m_endTexture, TextureWrap::Repeat, TextureWrap::Repeat);
            // End fog: the tinted average of the texture is ~the MC End haze.
            m_skyboxFogColor = kEndSkyTint * 0.35f;
            m_skyboxIsEnd = true;
            m_skyboxValid = true;
            return true;
        }

        const std::string dir = ResolveSkyboxDir(id);
        if (dir.empty()) return false;
        // A pack folder with an OptiFine sky rather than six faces: its
        // overworld layers go over the vanilla sky.
        if (SkyboxFacePath(dir, 0).empty()) {
            if (!m_pack.loaded || m_pack.id != id) {
                if (!LoadOptiFinePack(id, dir)) return false;
            }
            m_skyboxIsOptiFine = true;
            m_skyboxValid = true;
            return true;
        }

        glm::vec3 horizonAccum{0.0f};
        int horizonSamples = 0;
        bool allValid = true;
        for (int i = 0; i < 6; ++i) {
            // Horizon color from the 4 side faces only (up/down don't touch
            // the horizon).
            const bool sideFace = i < 4;
            m_skyboxFaces[i] = LoadSkyboxFace(SkyboxFacePath(dir, i),
                                              sideFace ? &horizonAccum : nullptr,
                                              &horizonSamples);
            if (m_skyboxFaces[i] == INVALID_TEXTURE) allValid = false;
        }
        if (!allValid) {
            DestroySkyboxTextures();
            return false;
        }
        m_skyboxFogColor = horizonSamples > 0 ? horizonAccum / static_cast<float>(horizonSamples)
                                              : glm::vec3(0.5f);
        m_skyboxValid = true;
        return true;
    }

    void SkyRenderer::SetSkybox(const std::string& id, int mode) {
        // The PLAYER's choice. It is remembered separately from the active sky
        // so that being in the End (which forces its own) neither reports nor
        // overwrites it, and so returning to the overworld restores it.
        m_userSkyboxId = id.empty() ? "vanilla" : id;
        m_userSkyboxMode = std::clamp(mode, 0, 2);
        if (m_pack.loaded && m_pack.id != m_userSkyboxId && g_renderBackend) DestroyOptiFinePack();
        ApplyDimensionSky();
    }

    void SkyRenderer::SetDimension(int rawDimensionId) {
        if (m_dimension == rawDimensionId) return;
        m_dimension = rawDimensionId;
        ApplyDimensionSky();
    }

    void SkyRenderer::ApplyDimensionSky() {
        // MC DimensionSpecialEffects: NETHER is SkyType.NONE (no sky drawn at
        // all, the world is just fog), END is SkyType.END (the static starfield
        // cube), OVERWORLD is NORMAL.
        m_noSky = (m_dimension == -1);
        // Neither the Nether nor the End has a day/night cycle.
        EnvironmentState::Get().SetConstantAmbientLight(m_dimension != 0);
        if (m_noSky) {
            DestroySkyboxTextures();
            EnvironmentState::Get().SetSkyboxOverride(
                true, glm::vec3(kNetherFog[0], kNetherFog[1], kNetherFog[2]),
                /*mode 0 = constant, no night curve*/ 0);
            return;
        }
        // The End forces its own sky; everywhere else honours the player.
        const bool inEnd = (m_dimension == 1);
        ApplySkybox(inEnd ? std::string("end") : m_userSkyboxId,
                    inEnd ? 0 : m_userSkyboxMode);
        // A chosen pack's End layers (world1) ride over the End starfield,
        // so the pack is made resident here too when it is not yet.
        if (inEnd && m_initialized && g_renderBackend && !PackChosen()) {
            std::string dir = ResolveSkyboxDir(m_userSkyboxId);
            if (!dir.empty() && SkyboxFacePath(dir, 0).empty()) LoadOptiFinePack(m_userSkyboxId, dir);
            else if (m_userSkyboxId == "vanilla" && !(dir = ResourcePackSkyRoot()).empty()) LoadOptiFinePack("vanilla", dir);
        }
    }

    void SkyRenderer::ApplySkybox(const std::string& id, int mode) {
        m_skyboxMode = std::clamp(mode, 0, 2);
        m_skyboxId = id.empty() ? "vanilla" : id;

        if (!m_initialized || !g_renderBackend) {
            return;  // applied lazily if Initialize runs later
        }
        if (m_skyboxId == "vanilla") {
            DestroySkyboxTextures();
            // "Vanilla" is what OptiFine draws: the vanilla sky, with the
            // custom sky of the top enabled resource pack over it when one
            // has it. The pack is loaded under the id it stands in for.
            const std::string root = ResourcePackSkyRoot();
            if (!root.empty()) {
                if (!m_pack.loaded || m_pack.dir != root) LoadOptiFinePack("vanilla", root);
                if (m_pack.loaded) { m_skyboxIsOptiFine = true; m_skyboxValid = true; }
            } else if (m_pack.loaded && m_pack.id == "vanilla") {
                DestroyOptiFinePack();
            }
            EnvironmentState::Get().SetSkyboxOverride(false, glm::vec3(0.5f), m_skyboxMode);
            return;
        }
        if (!LoadSkyboxTextures(m_skyboxId)) {
            Log::Warning("SkyRenderer: skybox '%s' not found — using vanilla sky",
                         m_skyboxId.c_str());
            m_skyboxId = "vanilla";
            EnvironmentState::Get().SetSkyboxOverride(false, glm::vec3(0.5f), m_skyboxMode);
            return;
        }
        // An OptiFine sky is layered over the vanilla sky, so the vanilla
        // fog and its day curve stay; a cube set replaces the sky and
        // brings its own horizon colour.
        EnvironmentState::Get().SetSkyboxOverride(!m_skyboxIsOptiFine, m_skyboxFogColor, m_skyboxMode);
        Log::Info("SkyRenderer: skybox '%s' (%s)", m_skyboxId.c_str(),
                  m_skyboxIsOptiFine ? "OptiFine layers" : ("mode " + std::to_string(m_skyboxMode)).c_str());
    }

    bool SkyRenderer::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) return false;

        m_shader = CreateSkyShader(vertexShaderSource, fragmentShaderSource);
        if (m_shader == INVALID_SHADER) {
            Log::Warning("SkyRenderer: failed to create shader — sky disabled");
            return false;
        }

        unsigned char white[] = {255, 255, 255, 255};
        m_whiteTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
        m_sunTexture = LoadEnvTexture("assets/textures/environment/sun.png");
        m_moonTexture = LoadEnvTexture("assets/textures/environment/moon_phases.png");

        BuildSkyDiscs();
        BuildSunriseFan();
        BuildCelestialQuads();
        BuildStars();
        BuildSkyboxCubes();

        m_initialized = true;
        Log::Info("SkyRenderer initialized");

        // A skybox selected before init (session start races renderer init
        // only in pathological orders — handle it anyway).
        if (m_skyboxId != "vanilla") {
            SetSkybox(m_skyboxId, m_skyboxMode);
        }
        return true;
    }

    void SkyRenderer::Shutdown() {
        if (!g_renderBackend) return;
        DestroySkyboxTextures();
        DestroyOptiFinePack();
        DestroyMesh(m_skyboxCube);
        DestroyMesh(m_endCube);
        DestroyMesh(m_topDisc);
        DestroyMesh(m_bottomDisc);
        DestroyMesh(m_sunriseFan);
        DestroyMesh(m_sunQuad);
        DestroyMesh(m_moonQuads);
        DestroyMesh(m_stars);
        if (m_whiteTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_whiteTexture); m_whiteTexture = INVALID_TEXTURE; }
        if (m_sunTexture != INVALID_TEXTURE)   { g_renderBackend->DestroyTexture(m_sunTexture);   m_sunTexture = INVALID_TEXTURE; }
        if (m_moonTexture != INVALID_TEXTURE)  { g_renderBackend->DestroyTexture(m_moonTexture);  m_moonTexture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)        { g_renderBackend->DestroyShader(m_shader);        m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void SkyRenderer::Render(const glm::mat4& proj, const glm::mat4& viewRotation) {
        if (!m_initialized || !g_renderBackend) return;
        // MC SkyType.NONE — the Nether has no sky, no sun, no moon and no
        // stars. The frame is already cleared to the fog colour, which is
        // exactly what vanilla shows there.
        if (m_noSky) return;

        const EnvironmentState& envState = EnvironmentState::Get();
        const EnvironmentFrame& env = envState.Frame();
        const glm::mat4 vp = proj * viewRotation;
        const float sunAngleRad = glm::radians(env.sunAngleDeg);

        PipelineState state;
        state.depthTestEnabled = false;
        state.depthWriteEnabled = false;
        state.blendEnabled = false;
        state.cullMode = CullMode::None;
        state.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);

        auto draw = [&](const Mesh& m, const glm::mat4& model, const glm::vec4& color,
                        TextureHandle tex, const glm::vec4& fogEnv,
                        uint32_t indexCount = 0, uint32_t indexOffset = 0) {
            g_renderBackend->BindTexture(tex != INVALID_TEXTURE ? tex : m_whiteTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", vp * model);
            g_renderBackend->SetUniformVec4(m_shader, "uColor", color);
            g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(env.fogColor, 1.0f));
            g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", fogEnv);
            g_renderBackend->DrawIndexed(m.mesh, indexCount ? indexCount : m.indexCount, indexOffset);
        };

        // MC sky fog: apply_fog(..., 0, FogSkyEnd, FogSkyEnd, FogSkyEnd, FogColor)
        const glm::vec4 skyFog{0.0f, env.fogSkyEnd, env.fogSkyEnd, env.fogSkyEnd};

        // ── Cubemap skybox path ─────────────────────────────────────────────
        // Replaces the sky disc / sunrise fan / dark disc. Mode 0 (static)
        // and 1 (darken) stop here; mode 2 continues into the celestial pass
        // so the sun/moon/stars ride on top of the skybox.
        const bool skyboxActive = m_skyboxValid && !m_skyboxIsOptiFine && m_skyboxId != "vanilla";
        if (skyboxActive) {
            const float brightness = (m_skyboxMode == 0) ? 1.0f : env.skyBrightness;
            RenderSkybox(vp, brightness);
            // The chosen pack's End layers (optifine/sky/world1) over the
            // End starfield.
            if (m_skyboxIsEnd && PackChosen()) RenderOptiFineLayers(m_pack.layers[1], vp, env);
            if (m_skyboxMode != 2) {
                g_renderBackend->UnbindMesh();
                PipelineState defaultState;
                defaultState.depthTestEnabled = true;
                defaultState.depthWriteEnabled = true;
                defaultState.blendEnabled = false;
                defaultState.cullMode = CullMode::Back;
                g_renderBackend->SetPipelineState(defaultState);
                return;
            }
        } else {
            // 1. Sky disc (opaque).
            draw(m_topDisc, glm::mat4(1.0f), glm::vec4(env.skyColor, 1.0f), m_whiteTexture, skyFog);

            // 2. Sunrise/sunset glow fan (translucent).
            if (env.sunriseColor.a > 0.001f) {
                state.blendEnabled = true;
                state.srcBlendFactor = BlendFactor::SrcAlpha;
                state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
                g_renderBackend->SetPipelineState(state);

                const float flip = std::sin(sunAngleRad) < 0.0f ? 180.0f : 0.0f;
                glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));
                model = glm::rotate(model, glm::radians(flip + 90.0f), glm::vec3(0, 0, 1));
                model = glm::scale(model, glm::vec3(1.0f, 1.0f, env.sunriseColor.a));
                draw(m_sunriseFan, model, env.sunriseColor, m_whiteTexture, kFogOff);
            }

            // 2b. OptiFine custom sky layers, between the sky and the sun,
            //     where OptiFine draws them.
            if (m_skyboxIsOptiFine && m_pack.loaded) RenderOptiFineLayers(m_pack.layers[0], vp, env);
        }

        // A pack's own definition may switch the vanilla bodies off (a
        // nebula with its own stars); only the overworld's sky has them.
        const bool packDecor = m_skyboxIsOptiFine && m_pack.loaded;
        const bool showSun   = !packDecor || m_pack.showSun[0];
        const bool showMoon  = !packDecor || m_pack.showMoon[0];
        const bool showStars = !packDecor || m_pack.showStars[0];
        // ...and may bring its own sun and moon textures.
        const TextureHandle sunTex  = (packDecor && m_pack.sunTexture  != INVALID_TEXTURE) ? m_pack.sunTexture  : m_sunTexture;
        const TextureHandle moonTex = (packDecor && m_pack.moonTexture != INVALID_TEXTURE) ? m_pack.moonTexture : m_moonTexture;

        // 3. Celestial bodies — additive "overlay" blend (SRC_ALPHA, ONE).
        state.blendEnabled = true;
        state.srcBlendFactor = BlendFactor::SrcAlpha;
        state.dstBlendFactor = BlendFactor::One;
        g_renderBackend->SetPipelineState(state);

        const glm::mat4 celestialBase =
            glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 1, 0));

        // Sun.
        if (showSun && sunTex != INVALID_TEXTURE) {
            glm::mat4 model = glm::rotate(celestialBase, sunAngleRad, glm::vec3(1, 0, 0));
            model = glm::translate(model, glm::vec3(0, kCelestialHeight, 0));
            model = glm::scale(model, glm::vec3(kSunSize, 1.0f, kSunSize));
            draw(m_sunQuad, model, glm::vec4(1.0f), sunTex, kFogOff);
        }

        // Moon (phase quad selected by index offset).
        if (showMoon && moonTex != INVALID_TEXTURE) {
            glm::mat4 model =
                glm::rotate(celestialBase, glm::radians(env.moonAngleDeg), glm::vec3(1, 0, 0));
            model = glm::translate(model, glm::vec3(0, kCelestialHeight, 0));
            model = glm::scale(model, glm::vec3(kMoonSize, 1.0f, kMoonSize));
            draw(m_moonQuads, model, glm::vec4(1.0f), moonTex, kFogOff,
                 6, static_cast<uint32_t>(env.moonPhase) * 6);
        }

        // Stars.
        if (showStars && env.starBrightness > 0.0f && m_stars.indexCount > 0) {
            const glm::mat4 model =
                glm::rotate(celestialBase, glm::radians(env.starAngleDeg), glm::vec3(1, 0, 0));
            const float b = env.starBrightness;
            draw(m_stars, model, glm::vec4(b, b, b, b), m_whiteTexture, kFogOff);
        }

        // 4. Dark disc below the horizon (renderDarkDisc: translate +12 y).
        //    Vanilla sky only — a skybox has its own bottom face.
        if (!skyboxActive && envState.ShouldRenderDarkDisc()) {
            state.blendEnabled = false;
            g_renderBackend->SetPipelineState(state);
            const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0, 12, 0));
            draw(m_bottomDisc, model, glm::vec4(0, 0, 0, 1), m_whiteTexture, skyFog);
        }

        g_renderBackend->UnbindMesh();

        // Restore default pipeline state for the terrain pass.
        PipelineState defaultState;
        defaultState.depthTestEnabled = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled = false;
        defaultState.cullMode = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

    void SkyRenderer::RenderOptiFineLayers(std::vector<OptiFineLayer>& layers, const glm::mat4& viewProj,
                                           const EnvironmentFrame& env) {
        if (layers.empty()) return;
        const EnvironmentState& envState = EnvironmentState::Get();
        const int64_t dayTime = envState.DayTime();
        const int timeOfDay = static_cast<int>(((dayTime % 24000) + 24000) % 24000);
        // MC's celestial angle as a fraction of a turn: our sun angle is
        // that fraction × 360 already (OptiFine: level.getTimeOfDay()).
        const float skyAngle = env.sunAngleDeg / 360.0f;
        // Weather (CustomSky.renderSky passes rain and thunder level).
        const float rain    = envState.RainLevel();
        const float thunder = envState.ThunderLevel();
        const auto  now     = std::chrono::steady_clock::now();

        PipelineState state;
        state.depthTestEnabled  = false;
        state.depthWriteEnabled = false;
        state.blendEnabled      = true;
        state.cullMode          = CullMode::None;
        state.primitiveType     = PrimitiveType::Triangles;

        for (OptiFineLayer& layer : layers) {
            // Days (CustomSkyLayer.isActive): the day index counts from the
            // layer's own fade-in, so a night layer spanning midnight is one
            // day, taken modulo daysLoop.
            if (!layer.days.empty()) {
                const long long rel = dayTime - (layer.fadeAlwaysOn ? 0 : layer.startFadeIn);
                long long day = rel / 24000;
                if (rel < 0 && rel % 24000 != 0) --day;
                const int loop = std::max(1, layer.daysLoop);
                const int idx  = static_cast<int>(((day % loop) + loop) % loop);
                if (!InRanges(layer.days, idx)) continue;
            }

            // Fade (CommonUtils.calculateFadeAlphaValue).
            float fade = 1.0f;
            if (!layer.fadeAlwaysOn) {
                if (InTimeInterval(timeOfDay, layer.endFadeIn, layer.startFadeOut)) {
                    fade = 1.0f;
                } else if (InTimeInterval(timeOfDay, layer.startFadeIn, layer.endFadeIn)) {
                    const int dur = std::max(1, CyclicDistance(layer.startFadeIn, layer.endFadeIn));
                    fade = static_cast<float>(CyclicDistance(layer.startFadeIn, timeOfDay)) / dur;
                } else if (InTimeInterval(timeOfDay, layer.startFadeOut, layer.endFadeOut)) {
                    const int dur = std::max(1, CyclicDistance(layer.startFadeOut, layer.endFadeOut));
                    fade = 1.0f - static_cast<float>(CyclicDistance(layer.startFadeOut, timeOfDay)) / dur;
                } else {
                    fade = 0.0f;
                }
            }

            // Biome / height (getPositionBrightness): 1 where the viewer's
            // block matches, 0 elsewhere, eased over `transition` seconds
            // the way OptiFine's SmoothFloat does (an exponential approach
            // with that time constant, snapped once it is within a hair).
            float position = 1.0f;
            if (!layer.biomes.empty() || !layer.heights.empty()) {
                bool raw = true;
                if (!layer.biomes.empty()) {
                    const bool listed = std::find(layer.biomes.begin(), layer.biomes.end(),
                                                  m_observerBiome) != layer.biomes.end();
                    raw = listed != layer.biomesNegated;
                }
                if (raw && !layer.heights.empty()) raw = InRanges(layer.heights, m_observerBlock.y);
                const float target = raw ? 1.0f : 0.0f;
                float dt = 0.0f;
                if (layer.lastEval.time_since_epoch().count() != 0) {
                    dt = std::clamp(std::chrono::duration<float>(now - layer.lastEval).count(), 0.0f, 1.0f);
                }
                layer.lastEval = now;
                if (!layer.positionInit || layer.transitionSec <= 0.0f) {
                    layer.positionBrightness = target;
                    layer.positionInit = true;
                } else {
                    const float f = std::min(1.0f, dt / layer.transitionSec);
                    layer.positionBrightness += (target - layer.positionBrightness) * f;
                    if (std::fabs(target - layer.positionBrightness) < 0.002f) layer.positionBrightness = target;
                }
                position = layer.positionBrightness;
            }

            // Weather (getWeatherBrightness): the strengths of the kinds
            // the layer shows in, summed.
            float weather = 0.0f;
            if (layer.weatherClear)   weather += 1.0f - rain;
            if (layer.weatherRain)    weather += rain - thunder;
            if (layer.weatherThunder) weather += thunder;
            weather = std::clamp(weather, 0.0f, 1.0f);

            const float alpha = std::clamp(fade * position * weather, 0.0f, 1.0f);
            if (alpha < 1e-4f) continue;

            // Blend (OptiBoxes' Blend table: blend function + shader colour).
            glm::vec4 color(1.0f, 1.0f, 1.0f, alpha);
            state.blendEnabled = true;
            const std::string& b = layer.blend;
            if      (b == "alpha")    { state.srcBlendFactor = BlendFactor::SrcAlpha;         state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha; }
            else if (b == "subtract") { state.srcBlendFactor = BlendFactor::OneMinusDstColor; state.dstBlendFactor = BlendFactor::Zero;  color = glm::vec4(alpha, alpha, alpha, 1.0f); }
            else if (b == "multiply") { state.srcBlendFactor = BlendFactor::DstColor;         state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha; color = glm::vec4(alpha); }
            else if (b == "dodge")    { state.srcBlendFactor = BlendFactor::One;              state.dstBlendFactor = BlendFactor::One;   color = glm::vec4(alpha, alpha, alpha, 1.0f); }
            else if (b == "burn")     { state.srcBlendFactor = BlendFactor::Zero;             state.dstBlendFactor = BlendFactor::OneMinusSrcColor; color = glm::vec4(alpha, alpha, alpha, 1.0f); }
            else if (b == "screen")   { state.srcBlendFactor = BlendFactor::One;              state.dstBlendFactor = BlendFactor::OneMinusSrcColor; color = glm::vec4(alpha, alpha, alpha, 1.0f); }
            else if (b == "overlay")  { state.srcBlendFactor = BlendFactor::DstColor;         state.dstBlendFactor = BlendFactor::SrcColor; color = glm::vec4(alpha, alpha, alpha, 1.0f); }
            else if (b == "replace")  { state.blendEnabled = false; }
            else                      { state.srcBlendFactor = BlendFactor::SrcAlpha;         state.dstBlendFactor = BlendFactor::One; }   // add
            g_renderBackend->SetPipelineState(state);

            // Rotation with the time of day (OptiFineSkyRenderer.getAngle):
            // one turn per day at speed 1 about the layer's axis, given in
            // world coordinates — the default 0 0 1 is the sun's own axis.
            glm::mat4 model(1.0f);
            if (layer.rotate) {
                float angleDayStart = 0.0f;
                if (layer.speed != std::round(layer.speed)) {
                    const long long day = (dayTime + 18000) / 24000;
                    const double perDay = std::fmod(static_cast<double>(layer.speed), 1.0);
                    angleDayStart = static_cast<float>(std::fmod(static_cast<double>(day) * perDay, 1.0));
                }
                const float angle = glm::radians(360.0f * (angleDayStart + skyAngle * layer.speed));
                model = glm::rotate(model, angle, glm::normalize(layer.axis));
            }
            model = glm::scale(model, glm::vec3(100.0f));

            g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * model);
            g_renderBackend->SetUniformVec4(m_shader, "uColor", color);
            g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(0.0f));
            g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", kFogOff);
            for (int i = 0; i < 6; ++i) {
                g_renderBackend->BindTexture(layer.faces[i], 0);
                g_renderBackend->DrawIndexed(m_skyboxCube.mesh, 6, static_cast<uint32_t>(i) * 6);
            }
        }
    }

    void SkyRenderer::RenderSkybox(const glm::mat4& viewProj, float brightness) {
        // Camera-centered inward cube (MC buildEndSky places faces at ±100;
        // the exact scale is irrelevant with depth write off).
        const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(100.0f));
        const glm::mat4 mvp = viewProj * model;
        const glm::vec3 tint = (m_skyboxIsEnd ? kEndSkyTint : glm::vec3(1.0f)) * brightness;

        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformVec4(m_shader, "uColor", glm::vec4(tint, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(0.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", kFogOff);

        if (m_skyboxIsEnd) {
            g_renderBackend->BindTexture(m_endTexture, 0);
            g_renderBackend->DrawIndexed(m_endCube.mesh, m_endCube.indexCount);
        } else {
            // One draw per face so each binds its own 2D texture (same
            // trick as PanoramaRenderer — no cubemap sampler needed).
            for (int i = 0; i < 6; ++i) {
                g_renderBackend->BindTexture(m_skyboxFaces[i], 0);
                g_renderBackend->DrawIndexed(m_skyboxCube.mesh, 6, static_cast<uint32_t>(i) * 6);
            }
        }
    }

    bool SkyRenderer::EnsureEndTexture() {
        if (m_endTexture != INVALID_TEXTURE) return true;
        if (!g_renderBackend) return false;
        m_endTexture = LoadEnvTexture("assets/textures/environment/end_sky.png");
        if (m_endTexture == INVALID_TEXTURE) return false;
        g_renderBackend->SetTextureWrap(m_endTexture, TextureWrap::Repeat, TextureWrap::Repeat);
        return true;
    }

    void SkyRenderer::ReloadResources() {
        if (!m_initialized || !g_renderBackend) return;
        if (m_sunTexture != INVALID_TEXTURE)  { g_renderBackend->DestroyTexture(m_sunTexture);  m_sunTexture = INVALID_TEXTURE; }
        if (m_moonTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_moonTexture); m_moonTexture = INVALID_TEXTURE; }
        m_sunTexture  = LoadEnvTexture("assets/textures/environment/sun.png");
        m_moonTexture = LoadEnvTexture("assets/textures/environment/moon_phases.png");
        // The End texture and the pack are rebuilt by re-resolving the sky.
        DestroySkyboxTextures();
        DestroyOptiFinePack();
        ApplyDimensionSky();
    }

    void SkyRenderer::RenderForDimension(int rawDimensionId, const glm::mat4& proj,
                                         const glm::mat4& viewRotation) {
        if (!m_initialized || !g_renderBackend) return;
        if (rawDimensionId == m_dimension) { Render(proj, viewRotation); return; }
        if (rawDimensionId == -1) return;   // MC SkyType.NONE — fog only

        const bool        savedNoSky   = m_noSky;
        const bool        savedValid   = m_skyboxValid;
        const bool        savedIsEnd   = m_skyboxIsEnd;
        const bool        savedIsPack  = m_skyboxIsOptiFine;
        const std::string savedId      = m_skyboxId;
        const int         savedMode    = m_skyboxMode;

        m_noSky = false;
        if (rawDimensionId == 1) {
            if (!EnsureEndTexture()) return;
            m_skyboxValid      = true;
            m_skyboxIsEnd      = true;
            m_skyboxIsOptiFine = false;
            m_skyboxId         = "end";
            m_skyboxMode       = 0;
        } else {
            // The overworld's sky is the vanilla one, with the chosen
            // pack's layers over it when the player picked a pack.
            m_skyboxValid      = false;
            m_skyboxIsEnd      = false;
            m_skyboxIsOptiFine = PackChosen();
            m_skyboxId         = "vanilla";
        }
        Render(proj, viewRotation);

        m_noSky            = savedNoSky;
        m_skyboxValid      = savedValid;
        m_skyboxIsEnd      = savedIsEnd;
        m_skyboxIsOptiFine = savedIsPack;
        m_skyboxId         = savedId;
        m_skyboxMode       = savedMode;
    }

} // namespace Render
