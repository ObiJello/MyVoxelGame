// File: src/client/renderer/shader/ShaderPipeline.cpp
#include "ShaderPipeline.hpp"
#include "client/renderer/environment/Lightmap.hpp"
#include "ShaderPackGlsl.hpp"

#include "../backend/RenderBackend.hpp"
#include "../core/Camera.hpp"
#include "../core/Frustum.hpp"
#include "../core/RenderOrigin.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "client/shader/ShaderOptions.hpp"
#include "client/shader/ShaderPacks.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace Render {

    namespace {

        namespace fs = std::filesystem;

        // Composite layout: colortex0..7 on units 0..7, then the rest.
        constexpr int kCompDepth0      = 8;
        constexpr int kCompNoise       = 9;
        constexpr int kCompShadow0     = 10;
        constexpr int kCompShadowColor = 11;
        constexpr int kCompDepth1      = 12;
        constexpr int kCompDepth2      = 13;
        constexpr int kCompLightmap    = 14;
        constexpr int kCompShadow1     = 15;

        // Gbuffers layout: 0 atlas/texture, 1 sprite table, 2 face map are
        // the engine renderers'; the pack's extras follow.
        constexpr int kGbufLightmap    = 3;
        constexpr int kGbufNormals     = 4;
        constexpr int kGbufSpecular    = 5;
        constexpr int kGbufNoise       = 6;
        constexpr int kGbufDepth0      = 7;
        constexpr int kGbufDepth1      = 8;
        constexpr int kGbufShadow0     = 9;
        constexpr int kGbufShadowColor = 10;
        constexpr int kGbufShadow1     = 11;
        constexpr int kGbufEntityMap   = 12;   // the block.properties map (EnsureEntityMap)
        constexpr int kGbufSpriteTable = 1;    // the engine's sprite table (ChunkRenderer::BindSpriteTable)

        constexpr int kMaxShadowRes = 8192;

        // ── Diagnostics (flip, build, test; both false when done) ──────
        //   kDebugEngineTerrain: the terrain and water passes draw with the
        //                        ENGINE's shaders into the pack's targets.
        //   kDebugNoComposites:  the composite and final passes are skipped
        //                        and colortex0 is shown as it stands.
        constexpr bool kDebugEngineTerrain = false;
        constexpr bool kDebugNoComposites  = false;

        bool ReadFileRaw(const fs::path& p, std::string& out) {
            std::ifstream in(p, std::ios::binary);
            if (!in) return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

        // A composite pass without its own vertex shader: MC's quad through
        // ftransform, the texcoord in gl_TexCoord[0] as packs expect.
        const char* kDefaultVertex =
            "#version 120\n"
            "void main() {\n"
            "    gl_Position = ftransform();\n"
            "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
            "}\n";

        TextureFormat FormatFor(const std::string& name) {
            if (name == "RGBA8" || name == "RGB8" || name == "R8" || name == "RG8" ||
                name == "SRGB8_ALPHA8" || name == "SRGB8" || name == "RGBA8_SNORM") {
                return TextureFormat::RGBA8;
            }
            if (name == "R11F_G11F_B10F") return TextureFormat::R11G11B10F;
            if (name.find("32F") != std::string::npos) return TextureFormat::RGBA32F;
            return TextureFormat::RGBA16F;
        }

        // Iris CelestialUniforms: skyAngle is MC's timeOfDay (0 = noon);
        // sunAngle puts 0 at sunrise, 0.25 at noon, 0.5 at sunset.
        float SunAngle(float sunAngleDeg) {
            float sky = sunAngleDeg / 360.0f;
            sky -= std::floor(sky);
            return sky < 0.75f ? sky + 0.25f : sky - 0.75f;
        }
        // Iris CelestialUniforms.getCelestialPosition: the sun is (0, 100, 0)
        // taken through Y(-90) * Z(sunPathRotation) * X(skyAngle * 360), the
        // rotation MC's renderSky applies. In world space that is
        // (-sin a, cos a cos s, -cos a sin s): rising in the east, tilted
        // toward +Z (south) for the negative sunPathRotation most packs use.
        glm::vec3 SunDirection(float sunAngleDeg, float sunPathRotation) {
            const glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                                glm::rotate(glm::mat4(1.0f), glm::radians(sunPathRotation), glm::vec3(0.0f, 0.0f, 1.0f)) *
                                glm::rotate(glm::mat4(1.0f), glm::radians(sunAngleDeg), glm::vec3(1.0f, 0.0f, 0.0f));
            return glm::normalize(glm::vec3(r * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
        }

        // `const <type> name = value;` in a source, as text.
        bool FindConst(const std::string& source, const char* name, std::string& value) {
            const std::regex re(std::string(R"(const[ \t]+(int|float|bool)[ \t]+)") + name + R"([ \t]*=[ \t]*([-+0-9.eEf]+|true|false)[ \t]*;)");
            std::smatch m;
            if (!std::regex_search(source, m, re)) return false;
            value = m[2].str();
            return true;
        }

        std::string Trim(std::string s) {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        // shaders.properties `alphaTest.<program> = off | GL_GREATER 0.1`.
        float AlphaRefFrom(const std::string& v, float def) {
            const std::string t = Trim(v);
            if (t == "off" || t == "false") return 0.0f;
            const size_t sp = t.find_last_of(" \t");
            try { return std::stof(sp == std::string::npos ? t : t.substr(sp + 1)); } catch (...) { return def; }
        }

        bool BlendFactorFrom(const std::string& name, BlendFactor& out) {
            static const std::pair<const char*, BlendFactor> kTable[] = {
                {"ZERO", BlendFactor::Zero}, {"ONE", BlendFactor::One},
                {"SRC_ALPHA", BlendFactor::SrcAlpha}, {"ONE_MINUS_SRC_ALPHA", BlendFactor::OneMinusSrcAlpha},
            };
            for (const auto& [n, f] : kTable) if (name == n || name == std::string("GL_") + n) { out = f; return true; }
            return false;
        }

    } // namespace

    ShaderPipeline& ShaderPipeline::Get() {
        static ShaderPipeline s_instance;
        return s_instance;
    }

    // ── loading ───────────────────────────────────────────────────────────

    void ShaderPipeline::ApplySelected() {
        const std::string id = Shaders::Selected();
        if (id.empty()) {
            Unload();
            m_status = "Off";
            return;
        }
        if (!g_renderBackend) return;
        if (g_renderBackend->GetType() != BackendType::OpenGL) {
            Unload();
            m_status = "Shader packs need the OpenGL backend (this session runs on Vulkan)";
            return;
        }
        Shaders::PackInfo pack;
        if (!Shaders::Find(id, pack)) {
            Unload();
            m_status = "Pack not found: " + id.substr(id.find('/') == std::string::npos ? 0 : id.find('/') + 1);
            return;
        }
        std::string root, error;
        if (!Shaders::Prepare(pack, root, error)) {
            Unload();
            m_status = error;
            return;
        }
        m_packName = pack.name;
        if (!Load(root, error)) {
            Unload();
            m_status = pack.name + ": " + error;
            return;
        }
        m_status = pack.name + ": " + std::to_string(m_passes.size() + m_deferred.size()) + " pass" +
                   (m_passes.size() + m_deferred.size() == 1 ? "" : "es") + ", " +
                   std::to_string(m_gbufferCount) + " gbuffers program" + (m_gbufferCount == 1 ? "" : "s") +
                   (m_gbuffers[kFamShadow].shader != INVALID_SHADER ? ", shadows" : "");
    }

    bool ShaderPipeline::ReadPackFile(const std::string& rel, std::string& out) const {
        std::string raw;
        if (!ReadFileRaw(fs::path(m_packRoot) / rel, raw)) return false;
        out = Shaders::Apply(raw, m_options);
        return true;
    }

    // The pack's options as the preprocessor sees them: a toggle is defined
    // or not; a value option is its number.
    std::string ShaderPipeline::PreprocessProperties(const std::string& text) const {
        using ShaderExpr::Value;
        ShaderExpr::Vars vars;
        vars["MC_VERSION"] = Value::Scalar(12104.0f);
        vars["MC_GL_VERSION"] = Value::Scalar(330.0f);
        vars["MC_GLSL_VERSION"] = Value::Scalar(330.0f);
        for (const auto& [name, on] : m_toggles) vars[name] = Value::Scalar(on ? 1.0f : 0.0f);
        for (const auto& [name, v] : m_optionValues) vars[name] = Value::Scalar(v);
        auto isDefined = [&](const std::string& name) {
            if (name == "MC_VERSION" || name == "MC_GL_VERSION" || name == "MC_GLSL_VERSION") return true;
#if defined(__APPLE__)
            if (name == "MC_OS_MAC") return true;
#elif defined(_WIN32)
            if (name == "MC_OS_WINDOWS") return true;
#else
            if (name == "MC_OS_LINUX") return true;
#endif
            auto t = m_toggles.find(name);
            if (t != m_toggles.end()) return t->second;
            return m_optionValues.count(name) > 0;
        };
        auto evalCondition = [&](std::string expr) {
            // defined(X) / defined X -> 1 / 0
            static const std::regex kDefined(R"(defined[ \t]*\(?[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\)?)");
            std::string out;
            std::sregex_iterator it(expr.begin(), expr.end(), kDefined), end;
            size_t last = 0;
            for (; it != end; ++it) {
                out += expr.substr(last, static_cast<size_t>(it->position()) - last);
                out += isDefined((*it)[1].str()) ? "1" : "0";
                last = static_cast<size_t>(it->position() + it->length());
            }
            out += expr.substr(last);
            ShaderExpr::Context ctx;
            ctx.vars = &vars;
            Value v;
            std::string err;
            if (!ShaderExpr::Evaluate(out, ctx, v, err)) return false;   // an unknown name: MC treats it as 0
            return v.v[0] != 0.0f;
        };
        struct Level { bool parentActive; bool active; bool taken; };
        std::vector<Level> stack;
        std::string result;
        std::istringstream in(text);
        std::string line;
        auto active = [&]() { return stack.empty() || stack.back().active; };
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string t = Trim(line);
            std::string word, rest;
            if (!t.empty() && t[0] == '#') {
                size_t i = 1;
                while (i < t.size() && std::isalpha(static_cast<unsigned char>(t[i]))) ++i;
                word = t.substr(1, i - 1);
                rest = Trim(t.substr(i));
            }
            if (word == "if" || word == "ifdef" || word == "ifndef") {
                const bool parent = active();
                bool cond = false;
                if (parent) cond = word == "if" ? evalCondition(rest) : (isDefined(rest) == (word == "ifdef"));
                stack.push_back({parent, parent && cond, cond});
            } else if (word == "elif") {
                if (!stack.empty()) {
                    Level& l = stack.back();
                    const bool cond = l.parentActive && !l.taken && evalCondition(rest);
                    l.active = cond;
                    l.taken = l.taken || cond;
                }
            } else if (word == "else") {
                if (!stack.empty()) {
                    Level& l = stack.back();
                    l.active = l.parentActive && !l.taken;
                    l.taken = true;
                }
            } else if (word == "endif") {
                if (!stack.empty()) stack.pop_back();
            } else if (active()) {
                result += line;
                result += '\n';
            }
        }
        return result;
    }

    void ShaderPipeline::ParseBlockProperties() {
        m_blockMappings.clear();
        m_haveBlockProperties = false;
        std::string text;
        if (!ReadFileRaw(fs::path(m_packRoot) / "block.properties", text)) return;
        m_haveBlockProperties = true;
        std::istringstream in(PreprocessProperties(text));
        std::string line, pending;
        while (std::getline(in, line)) {
            std::string t = Trim(line);
            if (!pending.empty()) { t = pending + " " + t; pending.clear(); }
            if (t.empty() || t[0] == '#') continue;
            if (t.back() == '\\') { pending = t.substr(0, t.size() - 1); continue; }
            const size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(t.substr(0, eq));
            if (key.rfind("block.", 0) != 0) continue;
            BlockMapping m;
            m.id = std::atoi(key.c_str() + 6);
            std::istringstream names(t.substr(eq + 1));
            std::string n;
            while (names >> n) m.entries.push_back(n);
            m_blockMappings.push_back(std::move(m));
        }
        m_entityMapAtlas = INVALID_TEXTURE;   // rebuild the map for this pack
    }

    // The atlas-space map (see the header). Built when the pack loads and
    // again whenever the atlas is rebuilt (a resource pack change).
    void ShaderPipeline::EnsureEntityMap() {
        if (!g_atlasBuilder || !g_renderBackend) return;
        const TextureHandle table = g_atlasBuilder->GetSpriteTableHandle();
        if (table == INVALID_TEXTURE || table == m_entityMapAtlas) return;
        const int aw = g_atlasBuilder->GetAtlasWidth(), ah = g_atlasBuilder->GetAtlasHeight();
        if (aw <= 0 || ah <= 0) return;
        constexpr int kCell = 4;
        const int w = (aw + kCell - 1) / kCell, h = (ah + kCell - 1) / kCell;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);

        // id per sprite: the mapped blocks' textures, a later line winning.
        std::map<uint16_t, int> idBySprite;
        auto mapState = [&](Game::BlockState state, int id) {
            const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(state);
            for (const Game::Element& e : model.elements) {
                for (const auto& [dir, face] : e.faces) {
                    const std::string key = model.ResolveTexture(face.textureRef);
                    if (key.empty() || key == "missingno") continue;
                    AtlasUVRect r;
                    if (g_atlasBuilder->GetUVRect(key, r)) idBySprite[r.spriteId] = id;
                }
            }
        };
        std::vector<BlockMapping> mappings = m_blockMappings;
        if (!m_haveBlockProperties) {
            // No block.properties: OptiFine's legacy numeric ids for the two
            // blocks old packs test for.
            mappings.push_back({8, {"water"}});
            mappings.push_back({10, {"lava"}});
        }
        for (const BlockMapping& m : mappings) {
            for (std::string entry : m.entries) {
                if (entry.rfind("minecraft:", 0) == 0) entry = entry.substr(10);
                if (entry.empty() || std::isdigit(static_cast<unsigned char>(entry[0]))) continue;   // legacy numeric ids
                if (entry.find(':') != std::string::npos && entry.find('=') == std::string::npos) continue;  // another namespace
                // name[:prop=value]*
                std::vector<std::pair<std::string, std::string>> filters;
                std::string name = entry;
                const size_t colon = entry.find(':');
                if (colon != std::string::npos) {
                    name = entry.substr(0, colon);
                    std::stringstream ss(entry.substr(colon + 1));
                    std::string part;
                    while (std::getline(ss, part, ':')) {
                        const size_t eq = part.find('=');
                        if (eq != std::string::npos) filters.emplace_back(part.substr(0, eq), part.substr(eq + 1));
                    }
                }
                const Game::BlockID id = Game::BlockStates::FromSlug(name).Block();
                if (id == Game::BlockID::Air && name != "air") continue;   // not a block this engine has
                const uint32_t count = Game::BlockStates::Count(id);
                for (uint32_t i = 0; i < count; ++i) {
                    const Game::BlockState state = Game::BlockStates::FromIndex(id, static_cast<Game::BlockStateIndex>(i));
                    bool ok = true;
                    for (const auto& [prop, value] : filters) {
                        if (state.GetValueByName(prop) != value) { ok = false; break; }
                    }
                    if (ok) mapState(state, m.id);
                }
            }
        }

        // Every sprite's row, and the mapped ones' ids, over the sprite's
        // padded cells (the atlas keeps 16 px of padding around each sprite,
        // so a vertex on a sprite edge lands in cells only it claims).
        g_atlasBuilder->ForEachUVRect([&](const std::string&, const AtlasUVRect& r) {
            constexpr int kSpread = 8;
            const int x0 = std::max(0, static_cast<int>(std::floor(r.uvMin.x * aw)) - kSpread) / kCell;
            const int y0 = std::max(0, static_cast<int>(std::floor(r.uvMin.y * ah)) - kSpread) / kCell;
            const int x1 = std::min(aw, static_cast<int>(std::ceil(r.uvMax.x * aw)) + kSpread - 1) / kCell;
            const int y1 = std::min(ah, static_cast<int>(std::ceil(r.uvMax.y * ah)) + kSpread - 1) / kCell;
            const auto idIt = idBySprite.find(r.spriteId);
            const int id = idIt != idBySprite.end() ? idIt->second : 0;
            const int sprite = static_cast<int>(r.spriteId) + 1;
            for (int y = y0; y <= std::min(y1, h - 1); ++y) {
                for (int x = x0; x <= std::min(x1, w - 1); ++x) {
                    uint8_t* t = &px[(static_cast<size_t>(y) * w + x) * 4];
                    t[0] = static_cast<uint8_t>(id & 255);
                    t[1] = static_cast<uint8_t>((id >> 8) & 255);
                    t[2] = static_cast<uint8_t>(sprite & 255);
                    t[3] = static_cast<uint8_t>((sprite >> 8) & 255);
                }
            }
        });

        if (m_entityMap != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_entityMap);
        m_entityMap = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px.data());
        g_renderBackend->SetTextureFilter(m_entityMap, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_entityMap, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        m_entityMapW = w;
        m_entityMapH = h;
        m_entityMapAtlas = table;
        // The programs read the map's size as a uniform.
        for (const Program& p : m_gbuffers) {
            if (p.shader == INVALID_SHADER || !p.ownsShader) continue;
            g_renderBackend->BindShader(p.shader);
            g_renderBackend->SetUniformVec2(p.shader, "sp_entityMapSize", glm::vec2(static_cast<float>(w), static_cast<float>(h)));
        }
        Log::Info("[ShaderPipeline] block map: %zu id(s) over %zu sprite(s), %dx%d cells", mappings.size(), idBySprite.size(), w, h);
    }

    void ShaderPipeline::ParseProperties() {
        m_props.clear();
        m_customUniforms.clear();
        m_smoothState.clear();
        std::string raw;
        if (!ReadFileRaw(fs::path(m_packRoot) / "shaders.properties", raw)) return;
        const std::string text = PreprocessProperties(raw);
        std::istringstream in(text);
        std::string line, pending;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string t = Trim(line);
            if (!pending.empty()) { t = pending + " " + t; pending.clear(); }
            if (t.empty() || t[0] == '#') continue;
            if (t.back() == '\\') { pending = t.substr(0, t.size() - 1); continue; }
            const size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(t.substr(0, eq));
            const std::string value = Trim(t.substr(eq + 1));
            m_props[key] = value;
            // uniform.<type>.<name> / variable.<type>.<name>
            const bool isUniform = key.rfind("uniform.", 0) == 0, isVariable = key.rfind("variable.", 0) == 0;
            if (isUniform || isVariable) {
                const size_t a = key.find('.'), b = key.find('.', a + 1);
                if (b != std::string::npos) {
                    CustomUniform u;
                    u.type = key.substr(a + 1, b - a - 1);
                    u.name = key.substr(b + 1);
                    u.expr = value;
                    u.variable = isVariable;
                    m_customUniforms.push_back(std::move(u));
                }
            }
        }
    }

    // The frame's values of the built-in uniforms a custom uniform may
    // read, then every custom expression in file order.
    void ShaderPipeline::EvaluateCustomUniforms(const ShaderFrameInput& in) {
        if (m_customUniforms.empty()) return;
        using ShaderExpr::Value;
        ShaderExpr::Vars vars;
        const glm::mat3 rot(in.view);
        const glm::vec3 sunDir = SunDirection(in.sunAngleDeg, m_sunPathRotation);
        const glm::vec3 sunPos = rot * (sunDir * 100.0f);
        const glm::vec3 upPos = rot * glm::vec3(0.0f, 100.0f, 0.0f);
        const float sunAngle = SunAngle(in.sunAngleDeg);
        const glm::vec3 cam(in.cameraPosition);
        auto v3 = [](const glm::vec3& v) { return Value::Vec(v.x, v.y, v.z); };
        vars["frameCounter"]        = Value::Scalar(static_cast<float>(m_frameCounter));
        vars["frameTime"]           = Value::Scalar(in.deltaSeconds);
        vars["frameTimeCounter"]    = Value::Scalar(m_frameTimeCounter);
        vars["worldTime"]           = Value::Scalar(static_cast<float>(((in.dayTime % 24000) + 24000) % 24000));
        vars["worldDay"]            = Value::Scalar(static_cast<float>(in.dayTime / 24000));
        vars["moonPhase"]           = Value::Scalar(static_cast<float>(in.moonPhase));
        vars["sunAngle"]            = Value::Scalar(sunAngle);
        vars["shadowAngle"]         = Value::Scalar(sunAngle < 0.5f ? sunAngle : sunAngle - 0.5f);
        vars["sunPosition"]         = v3(sunPos);
        vars["moonPosition"]        = v3(-sunPos);
        vars["shadowLightPosition"] = v3(sunAngle < 0.5f ? sunPos : -sunPos);
        vars["upPosition"]          = v3(upPos);
        vars["cameraPosition"]      = v3(cam);
        vars["previousCameraPosition"] = v3(m_havePrev ? glm::vec3(m_prevCameraPosition) : cam);
        vars["eyeAltitude"]         = Value::Scalar(cam.y);
        vars["eyeBrightness"]       = Value::Vec(static_cast<float>(std::clamp(in.eyeBlockLight, 0, 15) * 16),
                                                 static_cast<float>(std::clamp(in.eyeSkyLight, 0, 15) * 16), 0.0f, 0.0f, 2);
        vars["eyeBrightnessSmooth"] = Value::Vec(std::round(m_eyeBrightnessSmooth.x), std::round(m_eyeBrightnessSmooth.y), 0.0f, 0.0f, 2);
        vars["rainStrength"]        = Value::Scalar(0.0f);
        vars["wetness"]             = Value::Scalar(0.0f);
        vars["nightVision"]         = Value::Scalar(0.0f);
        vars["blindness"]           = Value::Scalar(0.0f);
        vars["darknessFactor"]      = Value::Scalar(0.0f);
        vars["darknessLightFactor"] = Value::Scalar(0.0f);
        vars["screenBrightness"]    = Value::Scalar(1.0f);
        vars["centerDepthSmooth"]   = Value::Scalar(1.0f);
        vars["isEyeInWater"]        = Value::Scalar(static_cast<float>(in.isEyeInWater));
        vars["hideGUI"]             = Value::Scalar(in.hideGui ? 1.0f : 0.0f);
        vars["heldItemId"]          = Value::Scalar(0.0f);
        vars["heldItemId2"]         = Value::Scalar(0.0f);
        vars["heldBlockLightValue"] = Value::Scalar(0.0f);
        vars["heldBlockLightValue2"] = Value::Scalar(0.0f);
        vars["viewWidth"]           = Value::Scalar(static_cast<float>(m_width));
        vars["viewHeight"]          = Value::Scalar(static_cast<float>(m_height));
        vars["aspectRatio"]         = Value::Scalar(static_cast<float>(m_width) / static_cast<float>(std::max(1, m_height)));
        vars["near"]                = Value::Scalar(in.nearPlane);
        vars["far"]                 = Value::Scalar(in.farPlane);
        vars["skyColor"]            = v3(in.skyColor);
        vars["fogColor"]            = v3(in.fogColor);
        vars["fogStart"]            = Value::Scalar(in.fogStart);
        vars["fogEnd"]              = Value::Scalar(in.fogEnd);
        vars["fogDensity"]          = Value::Scalar(0.0f);
        vars["fogMode"]             = Value::Scalar(9729.0f);
        // Biome values (OptiFine): a temperate default until biomes reach the client.
        vars["temperature"]         = Value::Scalar(0.8f);
        vars["rainfall"]            = Value::Scalar(0.4f);
        vars["biome"]               = Value::Scalar(0.0f);
        vars["biome_category"]      = Value::Scalar(0.0f);
        vars["biome_precipitation"] = Value::Scalar(1.0f);
        vars["renderStage"]         = Value::Scalar(0.0f);
        vars["PI"]                  = Value::Scalar(3.14159265f);

        ShaderExpr::Context ctx;
        ctx.vars = &vars;
        ctx.smooth = &m_smoothState;
        ctx.deltaSeconds = in.deltaSeconds;
        for (CustomUniform& u : m_customUniforms) {
            std::string err;
            Value v;
            if (!ShaderExpr::Evaluate(u.expr, ctx, v, err)) {
                if (!u.failed) Log::Warning("[ShaderPipeline] custom uniform %s: %s", u.name.c_str(), err.c_str());
                u.failed = true;
                v = Value::Scalar(0.0f);
            }
            if (u.type == "int" || u.type == "bool") v.v[0] = std::round(v.v[0]);
            u.value = v;
            vars[u.name] = v;
        }
    }

    // shaders.properties `program.<name>.enabled = <expr>`: a boolean
    // expression over the pack's toggle options (`Shadows || TAA`,
    // `!(A && B)`, `false`). Absent = enabled.
    bool ShaderPipeline::OptionIsOn(const std::string& name) const {
        if (name == "true") return true;
        if (name == "false") return false;
        auto it = m_toggles.find(name);
        return it != m_toggles.end() && it->second;
    }

    namespace {
        struct ExprParser {
            const std::string& s;
            size_t i = 0;
            const ShaderPipeline* self;
            bool (ShaderPipeline::*isOn)(const std::string&) const;
            void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
            bool primary() {
                ws();
                if (i < s.size() && s[i] == '!') { ++i; return !primary(); }
                if (i < s.size() && s[i] == '(') { ++i; const bool v = orExpr(); ws(); if (i < s.size() && s[i] == ')') ++i; return v; }
                const size_t start = i;
                while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
                return (self->*isOn)(s.substr(start, i - start));
            }
            bool andExpr() {
                bool v = primary();
                for (;;) { ws(); if (s.compare(i, 2, "&&") == 0) { i += 2; v = primary() && v; } else return v; }
            }
            bool orExpr() {
                bool v = andExpr();
                for (;;) { ws(); if (s.compare(i, 2, "||") == 0) { i += 2; v = andExpr() || v; } else return v; }
            }
        };
    }

    bool ShaderPipeline::ProgramEnabled(const std::string& name) const {
        auto it = m_props.find("program." + name + ".enabled");
        if (it == m_props.end()) return true;
        ExprParser parser{it->second, 0, this, &ShaderPipeline::OptionIsOn};
        return parser.orExpr();
    }

    std::string ShaderPipeline::DimensionFolder(int dimension) const {
        std::error_code ec;
        const std::string folder = "world" + std::to_string(dimension);
        const fs::path dir = fs::path(m_packRoot) / folder;
        if (fs::is_directory(dir, ec)) {
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".fsh") return folder + "/";
            }
        }
        return "";
    }

    bool ShaderPipeline::CompileProgram(const std::string& dir, const std::string& name, bool gbuffers, bool entityLayout,
                                        Program& out, std::string& error) {
        const std::string fragRel = dir + name + ".fsh";
        const std::string vertRel = dir + name + ".vsh";
        std::string frag, vert;
        if (!ReadPackFile(fragRel, frag)) return false;   // no such program
        const bool ownVertex = ReadPackFile(vertRel, vert);
        if (!ownVertex) {
            if (gbuffers) { error = name + " has no vertex shader"; return false; }
            vert = kDefaultVertex;
        }

        const PackGlsl::FileReader reader = [this](const std::string& rel, std::string& body) {
            return ReadPackFile(rel, body);
        };
        using PackGlsl::Stage;
        const Stage vs = !gbuffers ? Stage::Vertex : entityLayout ? Stage::EntityVertex : Stage::TerrainVertex;
        const Stage fsStage = !gbuffers ? Stage::Fragment : entityLayout ? Stage::EntityFragment : Stage::TerrainFragment;
        PackGlsl::Translated tv = PackGlsl::Translate(vert, vs, ownVertex ? vertRel : fragRel, reader);
        if (!tv.error.empty()) { error = name + ": " + tv.error; return false; }
        PackGlsl::Translated tf = PackGlsl::Translate(frag, fsStage, fragRel, reader);
        if (!tf.error.empty()) { error = name + ": " + tf.error; return false; }

        // Buffer formats and shadow constants declared anywhere in the sources.
        for (const std::string* src : {&tf.source, &tv.source}) {
            for (const PackGlsl::FormatDecl& d : PackGlsl::FindFormatDecls(*src)) {
                if (d.index >= 0 && d.index < kColorBuffers) m_color[d.index].format = FormatFor(d.format);
            }
            for (const PackGlsl::ClearDecl& d : PackGlsl::FindClearDecls(*src)) {
                if (d.index < 0 || d.index >= kColorBuffers) continue;
                if (d.hasClear) m_color[d.index].clear = d.clear;
                if (d.hasColor) m_color[d.index].clearColor = glm::vec4(d.color[0], d.color[1], d.color[2], d.color[3]);
            }
            std::string v;
            if (FindConst(*src, "shadowMapResolution", v)) m_shadowRes = std::clamp(std::atoi(v.c_str()), 0, kMaxShadowRes);
            if (FindConst(*src, "shadowDistance", v))      m_shadowDistance = std::max(16.0f, static_cast<float>(std::atof(v.c_str())));
            if (FindConst(*src, "sunPathRotation", v))     m_sunPathRotation = static_cast<float>(std::atof(v.c_str()));
        }

        out.name = name;
        out.shader = g_renderBackend->CreateShader(tv.source, tf.source);
        out.ownsShader = true;
        if (out.shader == INVALID_SHADER) {
            error = name + " failed to compile (see the log)";
            return false;
        }
        out.drawBuffers.clear();
        for (int b : tf.drawBuffers) {
            if (b < 0 || b >= kColorBuffers) continue;
            if (std::find(out.drawBuffers.begin(), out.drawBuffers.end(), b) != out.drawBuffers.end()) continue;
            out.drawBuffers.push_back(b);
            m_color[b].used = true;
        }
        if (out.drawBuffers.empty()) { out.drawBuffers.push_back(0); m_color[0].used = true; }

        auto at = m_props.find("alphaTest." + name);
        out.alphaRef = at != m_props.end() ? AlphaRefFrom(at->second, 0.1f) : 0.1f;
        g_renderBackend->BindShader(out.shader);
        g_renderBackend->SetUniformFloat(out.shader, "sp_alphaRef", out.alphaRef);
        return true;
    }

    // Every program of the pack for one dimension folder.
    bool ShaderPipeline::LoadPrograms(const std::string& dir, std::string& error) {
        DestroyPrograms();
        m_dir = dir;
        for (ColorBuffer& c : m_color) { c.format = TextureFormat::RGBA8; c.used = false; c.clear = true; c.clearColor = glm::vec4(0.0f); }
        m_color[0].used = true;
        m_shadowRes = 0;
        m_shadowDistance = 160.0f;
        m_sunPathRotation = 0.0f;
        std::error_code ec;
        const fs::path shaders = fs::path(m_packRoot) / dir;
        auto exists = [&](const std::string& name) { return fs::exists(shaders / (name + ".fsh"), ec); };

        // Composite chain and final.
        int disabled = 0;
        for (int i = 0; i < 100; ++i) {
            const std::string name = i == 0 ? "composite" : "composite" + std::to_string(i);
            if (!exists(name)) continue;
            if (!ProgramEnabled(name)) { ++disabled; continue; }
            Program p;
            std::string err;
            if (!CompileProgram(dir, name, false, false, p, err)) { error = err; return false; }
            m_passes.push_back(std::move(p));
        }
        if (exists("final") && ProgramEnabled("final")) {
            Program p;
            std::string err;
            if (!CompileProgram(dir, "final", false, false, p, err)) { error = err; return false; }
            p.isFinal = true;
            m_passes.push_back(std::move(p));
        }
        // Deferred chain, between the opaque and translucent stages.
        for (int i = 0; i < 100; ++i) {
            const std::string name = i == 0 ? "deferred" : "deferred" + std::to_string(i);
            if (!exists(name)) continue;
            if (!ProgramEnabled(name)) { ++disabled; continue; }
            Program p;
            std::string err;
            if (!CompileProgram(dir, name, false, false, p, err)) { error = err; return false; }
            m_deferred.push_back(std::move(p));
        }

        // The gbuffers programs, with Iris's fallback chain. A name that
        // several families resolve to is compiled once.
        struct FamilySpec { Family fam; std::vector<const char*> chain; bool entityLayout; };
        const FamilySpec specs[] = {
            {kFamTerrain,   {"gbuffers_terrain", "gbuffers_textured_lit", "gbuffers_textured", "gbuffers_basic"}, false},
            {kFamWater,     {"gbuffers_water", "gbuffers_terrain", "gbuffers_textured_lit", "gbuffers_textured", "gbuffers_basic"}, false},
            {kFamEntities,  {"gbuffers_entities", "gbuffers_textured_lit", "gbuffers_textured", "gbuffers_basic"}, true},
            {kFamBlock,     {"gbuffers_block", "gbuffers_entities", "gbuffers_textured_lit", "gbuffers_textured", "gbuffers_basic"}, true},
            {kFamSky,       {"gbuffers_skybasic", "gbuffers_skytextured", "gbuffers_textured", "gbuffers_basic"}, true},
            {kFamClouds,    {"gbuffers_clouds", "gbuffers_textured", "gbuffers_basic"}, true},
            {kFamParticles, {"gbuffers_textured", "gbuffers_textured_lit", "gbuffers_basic"}, true},
            {kFamShadow,    {"shadow"}, false},
        };
        std::map<std::string, Program> compiled;   // key: name + layout
        m_gbufferCount = 0;
        for (const FamilySpec& spec : specs) {
            std::string chosen;
            for (const char* n : spec.chain) if (exists(n) && ProgramEnabled(n)) { chosen = n; break; }
            if (chosen.empty()) continue;
            const std::string key = chosen + (spec.entityLayout ? "|entity" : "|terrain");
            auto it = compiled.find(key);
            if (it == compiled.end()) {
                Program p;
                std::string err;
                if (!CompileProgram(dir, chosen, true, spec.entityLayout, p, err)) { error = err; return false; }
                it = compiled.emplace(key, p).first;
                m_gbuffers[spec.fam] = p;
                ++m_gbufferCount;
            } else {
                m_gbuffers[spec.fam] = it->second;
                m_gbuffers[spec.fam].ownsShader = false;
            }
        }
        // The shadow program is not a gbuffers program for the count.
        if (m_gbuffers[kFamShadow].shader != INVALID_SHADER) --m_gbufferCount;
        if (m_shadowRes == 0 && m_gbuffers[kFamShadow].shader != INVALID_SHADER) m_shadowRes = 1024;

        // Sampler units: uniform values persist with the program. The block
        // map's size is set when the map is built (EnsureEntityMap).
        // MC_RENDER_STAGE_* for the `renderStage` uniform, per family.
        static const int kRenderStage[kFamilyCount] = { 8, 17, 11, 12, 1, 20, 19, 0 };
        for (int fam = 0; fam < kFamilyCount; ++fam) {
            const Program& p = m_gbuffers[fam];
            if (p.shader == INVALID_SHADER || !p.ownsShader) continue;
            g_renderBackend->BindShader(p.shader);
            SetSamplerUniforms(p, Layout::Gbuffers);
            g_renderBackend->SetUniformInt(p.shader, "renderStage", kRenderStage[fam]);
            g_renderBackend->SetUniformInt(p.shader, "sp_entityMap", kGbufEntityMap);
            g_renderBackend->SetUniformInt(p.shader, "sp_spriteTable", kGbufSpriteTable);
            g_renderBackend->SetUniformVec2(p.shader, "sp_entityMapSize", glm::vec2(0.0f));
            // Iris: an unmapped block reads -1; without a block.properties
            // the legacy ids stand in (0 for anything but water and lava).
            g_renderBackend->SetUniformFloat(p.shader, "sp_unmappedEntity", m_haveBlockProperties ? -1.0f : 0.0f);
        }
        m_entityMapAtlas = INVALID_TEXTURE;   // new programs: build (and size) the map again
        for (std::vector<Program>* list : {&m_passes, &m_deferred}) {
            for (const Program& p : *list) {
                g_renderBackend->BindShader(p.shader);
                SetSamplerUniforms(p, Layout::Composite);
            }
        }
        if (m_passes.empty() && m_deferred.empty() && m_gbufferCount == 0) {
            error = "no gbuffers, composite or final program";
            return false;
        }
        if (disabled > 0) Log::Info("[ShaderPipeline] %d program(s) disabled by shaders.properties for the current options", disabled);
        return true;
    }

    void ShaderPipeline::DestroyPrograms() {
        if (g_renderBackend) {
            for (std::vector<Program>* list : {&m_passes, &m_deferred}) {
                for (Program& p : *list) {
                    if (p.target != INVALID_RENDER_TARGET) g_renderBackend->DestroyRenderTarget(p.target);
                    if (p.shader != INVALID_SHADER && p.ownsShader) g_renderBackend->DestroyShader(p.shader);
                }
            }
            for (Program& p : m_gbuffers) {
                if (p.target != INVALID_RENDER_TARGET) g_renderBackend->DestroyRenderTarget(p.target);
                if (p.shader != INVALID_SHADER && p.ownsShader) g_renderBackend->DestroyShader(p.shader);
                p = Program{};
            }
        }
        m_passes.clear();
        m_deferred.clear();
        m_gbufferCount = 0;
        for (auto& v : m_familyEngineShaders) v.clear();
    }

    bool ShaderPipeline::Load(const std::string& packRoot, std::string& error) {
        Unload();
        if (!g_renderBackend) { error = "no render backend"; return false; }
        if (g_renderBackend->GetType() != BackendType::OpenGL) {
            error = "shader packs need the OpenGL backend";
            return false;
        }
        m_packRoot = packRoot;
        m_options = Shaders::LoadOverrides(m_packName);
        m_toggles.clear();
        m_optionValues.clear();
        for (const Shaders::Option& o : Shaders::Discover(m_packRoot)) {
            if (o.isToggle) {
                m_toggles[o.name] = Shaders::CurrentValue(o, m_options) == "ON";
            } else {
                const std::string v = Shaders::CurrentValue(o, m_options);
                char* end = nullptr;
                const float f = std::strtof(v.c_str(), &end);
                if (end != v.c_str()) m_optionValues[o.name] = f;
            }
        }
        ParseProperties();
        ParseBlockProperties();
        m_dimension = m_frame.dimension;
        if (!LoadPrograms(DimensionFolder(m_dimension), error)) { DestroyPrograms(); return false; }

        EnsureQuad();
        EnsureStaticTextures();
        // Greedy meshing goes off on the first frame (BeginScene).
        m_active = true;
        m_havePrev = false;
        m_haveShadow = false;
        m_frameCounter = 0;   // the diagnostic probes run again for this load
        Log::Info("[ShaderPipeline] %s: %zu composite, %zu deferred, %d gbuffers program(s)%s (folder '%s')",
                  m_packName.c_str(), m_passes.size(), m_deferred.size(), m_gbufferCount,
                  m_gbuffers[kFamShadow].shader != INVALID_SHADER ? ", shadow pass" : "", m_dir.c_str());
        return true;
    }

    void ShaderPipeline::Unload() {
        if (g_renderBackend) {
            RemoveOverrides();
            DestroyShadowBuffers();
            DestroyBuffers();
            DestroyPrograms();
        }
        if (m_active && g_chunkRenderer) g_chunkRenderer->SetGreedyMeshingEnabled(m_greedyWasEnabled);
        if (g_renderBackend && m_entityMap != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_entityMap); m_entityMap = INVALID_TEXTURE; }
        m_entityMapAtlas = INVALID_TEXTURE;
        m_haveEyeSmooth = false;
        m_active = false;
        m_inScene = false;
        m_packRoot.clear();
        m_props.clear();
        m_options.clear();
    }

    // ── GPU resources ─────────────────────────────────────────────────────

    void ShaderPipeline::EnsureQuad() {
        if (m_quad != INVALID_MESH) return;
        // MC / Iris draw the composite quad from (0,0) to (1,1) under an
        // orthographic 0..1 projection, so a pack's `gl_Vertex.xy * 2 - 1`
        // and its `ftransform()` both land on the screen.
        struct V { float x, y, z; float u, v; uint8_t r, g, b, a; };
        static_assert(sizeof(V) == 24, "vertex stride must match the block layout");
        const V verts[4] = {
            {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
            {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255},
            {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
            {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
        };
        const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        m_quadVb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, sizeof(verts), verts);
        m_quadIb = g_renderBackend->CreateBuffer(BufferUsage::Index, sizeof(idx), idx);
        m_quad   = g_renderBackend->CreateMesh(m_quadVb, m_quadIb, GetBlockVertexLayout());
        Log::Info("[ShaderPipeline] full-screen quad: mesh %u (vb %u, ib %u)",
                  static_cast<unsigned>(m_quad), static_cast<unsigned>(m_quadVb), static_cast<unsigned>(m_quadIb));
        if (m_quad == INVALID_MESH) Log::Error("[ShaderPipeline] the full-screen quad mesh could not be created");
    }

    void ShaderPipeline::EnsureStaticTextures() {
        if (m_noise == INVALID_TEXTURE) {
            // MC's noisetex: 256x256 of uniform noise, tiled.
            constexpr int n = 256;
            std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4);
            uint32_t state = 0x9E3779B9u;
            for (uint8_t& b : px) {
                state = state * 1664525u + 1013904223u;
                b = static_cast<uint8_t>(state >> 24);
            }
            m_noise = g_renderBackend->CreateTexture2D(n, n, TextureFormat::RGBA8, px.data());
            g_renderBackend->SetTextureFilter(m_noise, TextureFilter::Linear, TextureFilter::Linear);
            g_renderBackend->SetTextureWrap(m_noise, TextureWrap::Repeat, TextureWrap::Repeat);
        }
        if (m_whiteDepth == INVALID_TEXTURE) {
            // Depth 1.0 everywhere: a shadow lookup finds nothing nearer.
            const uint32_t depthOne = 0xFFFFFF00u;   // 24-bit depth in the high bits, stencil 0
            m_whiteDepth = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::Depth24Stencil8, &depthOne);
        }
        if (m_white == INVALID_TEXTURE) {
            const uint8_t white[4] = {255, 255, 255, 255};
            m_white = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
        }
        if (m_flatNormal == INVALID_TEXTURE) {
            // LabPBR: a flat normal map texel, no height, no material.
            const uint8_t flat[4] = {128, 128, 255, 255};
            m_flatNormal = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, flat);
        }
        if (m_lightmap == INVALID_TEXTURE) {
            m_lightmap = g_renderBackend->CreateTexture2D(16, 16, TextureFormat::RGBA8, nullptr);
            g_renderBackend->SetTextureFilter(m_lightmap, TextureFilter::Linear, TextureFilter::Linear);
            g_renderBackend->SetTextureWrap(m_lightmap, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            m_lightmapUploaded = false;
        }
        if (m_blit == INVALID_SHADER) {
            m_blit = g_renderBackend->CreateShader(
                "#version 330 core\n"
                "layout(location = 0) in vec3 aPos; layout(location = 1) in vec2 aUV;\n"
                "out vec2 vUV; void main() { vUV = aUV; gl_Position = vec4(aPos.xy * 2.0 - 1.0, 0.0, 1.0); gl_ClipDistance[0] = 1.0; }\n",
                "#version 330 core\n"
                "uniform sampler2D colortex0; in vec2 vUV; out vec4 frag;\n"
                "void main() { frag = texture(colortex0, vUV); }\n");
        }
    }

    // The pack's `lightmap` sampler: the engine's own lightmap texels
    // (Render::Lightmap — MC's LightTexture, x = block light, y = sky light,
    // the layout packs expect), uploaded when they change.
    void ShaderPipeline::UpdateLightmap(float skyBrightness) {
        (void)skyBrightness;
        if (m_lightmap == INVALID_TEXTURE) return;
        const auto& texels = Lightmap::Get().Texels();
        if (m_lightmapUploaded && texels == m_lightmapTexels) return;
        m_lightmapTexels = texels;
        m_lightmapUploaded = true;
        g_renderBackend->UpdateTexture2D(m_lightmap, 0, 0, 16, 16, m_lightmapTexels.data());
    }

    void ShaderPipeline::CreateBuffers(int width, int height) {
        DestroyBuffers();
        m_width = width;
        m_height = height;
        for (int i = 0; i < kColorBuffers; ++i) {
            ColorBuffer& c = m_color[i];
            if (i != 0 && !c.used) continue;
            c.main = g_renderBackend->CreateTexture2D(width, height, c.format, nullptr);
            c.alt  = g_renderBackend->CreateTexture2D(width, height, c.format, nullptr);
            for (TextureHandle t : {c.main, c.alt}) {
                g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
        }
        auto depthTexture = [&](int w, int h) {
            const TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::Depth24Stencil8, nullptr);
            g_renderBackend->SetTextureFilter(t, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            return t;
        };
        m_depth  = depthTexture(width, height);
        m_depth1 = depthTexture(width, height);
        m_depth2 = depthTexture(width, height);

        m_depth1Target = g_renderBackend->CreateRenderTargetFromTextures(nullptr, 0, m_depth1);
        m_depth2Target = g_renderBackend->CreateRenderTargetFromTextures(nullptr, 0, m_depth2);

        RebuildGbufferTargets();
        // The buffers kept across frames start defined: both copies cleared
        // once here, never again (a fresh texture's content is undefined).
        for (int i = 1; i < kColorBuffers; ++i) {
            const ColorBuffer& c = m_color[i];
            if (c.main == INVALID_TEXTURE || c.clear) continue;
            const TextureHandle both[2] = { c.main, c.alt };
            const RenderTargetHandle rt = g_renderBackend->CreateRenderTargetFromTextures(both, 2, INVALID_TEXTURE);
            if (rt == INVALID_RENDER_TARGET) continue;
            g_renderBackend->BindRenderTarget(rt);
            g_renderBackend->SetClearColor(c.clearColor.r, c.clearColor.g, c.clearColor.b, c.clearColor.a);
            g_renderBackend->Clear(true, false, false);
            g_renderBackend->DestroyRenderTarget(rt);
        }
        if (m_gbuffers[kFamShadow].shader != INVALID_SHADER && m_shadowRes > 0) CreateShadowBuffers();
    }

    // The gbuffers programs write their DRAWBUFFERS' main textures with
    // the live depth. Rebuilt after the deferred passes flip buffers.
    void ShaderPipeline::RebuildGbufferTargets() {
        for (int fam = 0; fam < kFamilyCount; ++fam) {
            Program& p = m_gbuffers[fam];
            if (fam == kFamShadow) continue;   // its target is the shadow map, owned by CreateShadowBuffers
            if (p.target != INVALID_RENDER_TARGET) { g_renderBackend->DestroyRenderTarget(p.target); p.target = INVALID_RENDER_TARGET; }
            if (p.shader == INVALID_SHADER) continue;
            std::vector<TextureHandle> outs;
            for (int idx : p.drawBuffers) outs.push_back(m_color[idx].main);
            p.target = g_renderBackend->CreateRenderTargetFromTextures(outs.data(), static_cast<int>(outs.size()), m_depth);
        }
        // The engine's own draws land in colortex0's CURRENT main texture,
        // and the per-frame clear covers the CURRENT mains of the rest —
        // except the buffers the pack keeps across frames (colortexNClear
        // = false: TAA and exposure histories), one group per clear colour.
        if (m_sceneTarget != INVALID_RENDER_TARGET) g_renderBackend->DestroyRenderTarget(m_sceneTarget);
        m_sceneTarget = g_renderBackend->CreateRenderTargetFromTextures(&m_color[0].main, 1, m_depth);
        for (ClearGroup& g : m_clearGroups) g_renderBackend->DestroyRenderTarget(g.target);
        m_clearGroups.clear();
        std::vector<std::vector<TextureHandle>> groupTextures;
        for (int i = 1; i < kColorBuffers; ++i) {
            const ColorBuffer& c = m_color[i];
            if (c.main == INVALID_TEXTURE || !c.clear) continue;
            size_t g = 0;
            while (g < m_clearGroups.size() && m_clearGroups[g].color != c.clearColor) ++g;
            if (g == m_clearGroups.size()) { m_clearGroups.push_back({INVALID_RENDER_TARGET, c.clearColor}); groupTextures.emplace_back(); }
            groupTextures[g].push_back(c.main);
        }
        for (size_t g = 0; g < m_clearGroups.size(); ++g) {
            m_clearGroups[g].target = g_renderBackend->CreateRenderTargetFromTextures(
                groupTextures[g].data(), static_cast<int>(groupTextures[g].size()), INVALID_TEXTURE);
        }
    }

    void ShaderPipeline::DestroyBuffers() {
        auto drop = [&](RenderTargetHandle& rt) {
            if (rt != INVALID_RENDER_TARGET) { g_renderBackend->DestroyRenderTarget(rt); rt = INVALID_RENDER_TARGET; }
        };
        drop(m_sceneTarget);
        for (ClearGroup& g : m_clearGroups) drop(g.target);
        m_clearGroups.clear();
        drop(m_depth1Target);
        drop(m_depth2Target);
        for (Program& p : m_gbuffers) if (&p != &m_gbuffers[kFamShadow]) drop(p.target);
        for (Program& p : m_passes)   drop(p.target);
        for (Program& p : m_deferred) drop(p.target);
        for (ColorBuffer& c : m_color) {
            if (c.main != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(c.main); c.main = INVALID_TEXTURE; }
            if (c.alt  != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(c.alt);  c.alt  = INVALID_TEXTURE; }
        }
        for (TextureHandle* t : {&m_depth, &m_depth1, &m_depth2}) {
            if (*t != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(*t); *t = INVALID_TEXTURE; }
        }
        m_width = m_height = 0;
    }

    void ShaderPipeline::CreateShadowBuffers() {
        DestroyShadowBuffers();
        const int res = m_shadowRes;
        auto depthTexture = [&]() {
            const TextureHandle t = g_renderBackend->CreateTexture2D(res, res, TextureFormat::Depth24Stencil8, nullptr);
            g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
            g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            return t;
        };
        m_shadowDepth0 = depthTexture();
        m_shadowDepth1 = depthTexture();
        m_shadowColor0 = g_renderBackend->CreateTexture2D(res, res, TextureFormat::RGBA8, nullptr);
        g_renderBackend->SetTextureFilter(m_shadowColor0, TextureFilter::Linear, TextureFilter::Linear);
        g_renderBackend->SetTextureWrap(m_shadowColor0, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        m_shadowTarget  = g_renderBackend->CreateRenderTargetFromTextures(&m_shadowColor0, 1, m_shadowDepth0);
        m_shadow1Target = g_renderBackend->CreateRenderTargetFromTextures(nullptr, 0, m_shadowDepth1);
        m_gbuffers[kFamShadow].target = m_shadowTarget;
        if (m_shadowTarget == INVALID_RENDER_TARGET) {
            Log::Warning("[ShaderPipeline] no shadow map at %d px; shadows off", res);
            DestroyShadowBuffers();
        }
    }

    void ShaderPipeline::DestroyShadowBuffers() {
        auto drop = [&](RenderTargetHandle& rt) {
            if (rt != INVALID_RENDER_TARGET) { g_renderBackend->DestroyRenderTarget(rt); rt = INVALID_RENDER_TARGET; }
        };
        drop(m_shadowTarget);
        drop(m_shadow1Target);
        m_gbuffers[kFamShadow].target = INVALID_RENDER_TARGET;
        for (TextureHandle* t : {&m_shadowDepth0, &m_shadowDepth1, &m_shadowColor0}) {
            if (*t != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(*t); *t = INVALID_TEXTURE; }
        }
        m_haveShadow = false;
    }

    // ── overrides ─────────────────────────────────────────────────────────

    void ShaderPipeline::InstallOverrides() {
        RenderBackend& b = *g_renderBackend;
        // The engine's programs each family stands in for, matched by what
        // their sources declare (the renderers are untouched). Rescanned
        // now and then: a renderer builds its program on first use.
        if (m_frameCounter % 30 == 0 || m_familyEngineShaders[kFamEntities].empty()) {
            auto has = [](const std::string& s, const char* t) { return s.find(t) != std::string::npos; };
            m_familyEngineShaders[kFamEntities] = b.FindShadersBySource([&](const std::string& vs, const std::string& fs) {
                return !has(vs, "aPosSlot") && (has(vs, "uEntityClipPlane") || (has(vs, "fragWorldPos") && has(fs, "uTextureAtlas")));
            });
            m_familyEngineShaders[kFamBlock] = b.FindShadersBySource([&](const std::string& vs, const std::string& fs) {
                return !has(vs, "aPosSlot") && !has(vs, "uModel") && has(fs, "uTex;");
            });
            m_familyEngineShaders[kFamSky] = b.FindShadersBySource([&](const std::string& vs, const std::string& fs) {
                return !has(vs, "aPosSlot") && !has(vs, "uModel") && has(fs, "uTexture;") && has(fs, "uFogEnv");
            });
            m_familyEngineShaders[kFamClouds] = b.FindShadersBySource([&](const std::string& vs, const std::string& fs) {
                return !has(vs, "aPosSlot") && has(vs, "uModel") && has(fs, "uColor;") && !has(fs, "uTextureAtlas") && !has(fs, "uTexture;");
            });
            m_familyEngineShaders[kFamParticles] = b.FindShadersBySource([&](const std::string& vs, const std::string& fs) {
                return !has(vs, "aPosSlot") && has(fs, "uSprite;");
            });
        }
        for (int fam = kFamEntities; fam <= kFamParticles; ++fam) {
            const Program& p = m_gbuffers[fam];
            for (ShaderHandle engine : m_familyEngineShaders[fam]) {
                b.SetShaderOverride(engine, p.shader, p.shader != INVALID_SHADER ? p.target : m_sceneTarget);
            }
        }
        // The pack programs carry their own targets on bind.
        for (const Program& p : m_gbuffers) {
            if (p.shader != INVALID_SHADER && p.target != INVALID_RENDER_TARGET) b.SetShaderOverride(p.shader, p.shader, p.target);
        }
        // Terrain and water: the chunk renderer's pass overrides.
        if (g_chunkRenderer) {
            const bool shadow = m_inShadowPass;
            for (int pass = 0; pass < 3; ++pass) {
                ChunkRenderer::PassOverride o;
                const Program& p = shadow ? m_gbuffers[kFamShadow]
                                          : m_gbuffers[pass == ChunkRenderer::kPassTranslucent ? kFamWater : kFamTerrain];
                o.shader = ((kDebugEngineTerrain || m_abEngineTerrain) && !shadow) ? INVALID_SHADER : p.shader;
                o.target = p.shader != INVALID_SHADER ? p.target : (shadow ? m_shadowTarget : m_sceneTarget);
                g_chunkRenderer->SetPassOverride(static_cast<ChunkRenderer::TerrainPass>(pass), o);
            }
            g_chunkRenderer->SetAfterPassesTarget(shadow ? m_shadowTarget : m_sceneTarget);
            g_chunkRenderer->SetBeforeTranslucentHook([this]() {
                if (!g_renderBackend) return;
                if (m_inShadowPass) {
                    // shadowtex1: the shadow depth without the translucents.
                    if (m_shadowTarget != INVALID_RENDER_TARGET && m_shadow1Target != INVALID_RENDER_TARGET) {
                        g_renderBackend->BlitRenderTargetDepth(m_shadowTarget, m_shadow1Target);
                    }
                    return;
                }
                // depthtex1: the depth before the translucents; then the
                // deferred passes.
                if (m_sceneTarget != INVALID_RENDER_TARGET && m_depth1Target != INVALID_RENDER_TARGET) {
                    g_renderBackend->BlitRenderTargetDepth(m_sceneTarget, m_depth1Target);
                }
                // depthtex2 too: MC's "before the hand" copy is taken after the
                // solid hand has been drawn into depthtex0, so a pack's hand
                // mask (depthtex2 > depthtex0) is false everywhere else. Our
                // hand is never in this depth, so the copy is made now, before
                // the deferred passes read it, and again at the end.
                if (m_sceneTarget != INVALID_RENDER_TARGET && m_depth2Target != INVALID_RENDER_TARGET) {
                    g_renderBackend->BlitRenderTargetDepth(m_sceneTarget, m_depth2Target);
                }
                if ((m_frameCounter % 300 >= 30 && m_frameCounter % 300 <= 33) || m_abEngineTerrain) {
                    ProbeThree(m_abEngineTerrain ? "after terrain, ENGINE shader (A/B)" : "after terrain, pack shader");
                    if (!m_abEngineTerrain) {
                        ProbeTexture(m_color[1].main, "after terrain, colortex1 (lightmap x,y / mats)");
                        ProbeNormals(m_frame);
                    }
                }
                RunDeferred();
            });
        }
    }

    void ShaderPipeline::RemoveOverrides() {
        if (g_renderBackend) g_renderBackend->ClearShaderOverrides();
        if (g_chunkRenderer) {
            g_chunkRenderer->ClearDirectionalView();
            g_chunkRenderer->ClearPassOverrides();
            g_chunkRenderer->SetAfterPassesTarget(INVALID_RENDER_TARGET);
        }
    }

    // ── per frame ─────────────────────────────────────────────────────────

    void ShaderPipeline::SetSuspended(bool suspended) {
        if (m_suspended == suspended) return;
        m_suspended = suspended;
        if (suspended && m_active) RemoveOverrides();
    }

    void ShaderPipeline::BeginScene(int width, int height) {
        if (!m_active || m_suspended || !g_renderBackend || width <= 0 || height <= 0) return;
        // A dimension change: the pack's folder for it (world-1, world1).
        if (m_frame.dimension != m_dimension) {
            const std::string dir = DimensionFolder(m_frame.dimension);
            m_dimension = m_frame.dimension;
            if (dir != m_dir) {
                std::string error;
                RemoveOverrides();
                DestroyShadowBuffers();
                DestroyBuffers();
                if (!LoadPrograms(dir, error)) {
                    Log::Error("[ShaderPipeline] %s", error.c_str());
                    Unload();
                    m_status = m_packName + ": " + error;
                    return;
                }
            }
        }
        if (width != m_width || height != m_height || m_sceneTarget == INVALID_RENDER_TARGET) {
            CreateBuffers(width, height);
            if (m_sceneTarget == INVALID_RENDER_TARGET) {
                Log::Error("[ShaderPipeline] could not create the scene target; shaders off");
                Unload();
                m_status = "Could not create the scene render target";
                return;
            }
        }
        m_inShadowPass = false;
        m_abEngineTerrain = m_frameCounter >= 40 && m_frameCounter <= 43;
        // Per-vertex attributes need per-block quads: greedy meshing off
        // while a pack is active. Checked here rather than only at load,
        // because the chunk renderer does not exist yet when the selected
        // pack loads at startup, and a new session gets a new renderer.
        if (g_chunkRenderer && g_chunkRenderer->IsGreedyMeshingEnabled()) {
            m_greedyWasEnabled = true;
            g_chunkRenderer->SetGreedyMeshingEnabled(false);
            Log::Info("[ShaderPipeline] greedy meshing off for the shader pack; remeshing");
        }
        g_renderBackend->CheckErrors("ShaderPipeline.BeginScene.before");
        InstallOverrides();
        if (m_frameCounter == 0 || m_frameCounter == 120) {
            Log::Info("[ShaderPipeline] frame %d: scene %dx%d, targets scene=%u clearGroups=%u depth1=%u depth2=%u shadow=%u (%d px)",
                      m_frameCounter, m_width, m_height,
                      static_cast<unsigned>(m_sceneTarget), static_cast<unsigned>(m_clearGroups.size()),
                      static_cast<unsigned>(m_depth1Target), static_cast<unsigned>(m_depth2Target),
                      static_cast<unsigned>(m_shadowTarget), m_shadowRes);
            Log::Info("[ShaderPipeline]   gbuffer targets terrain=%u water=%u entities=%u block=%u sky=%u clouds=%u particles=%u shadow=%u",
                      static_cast<unsigned>(m_gbuffers[kFamTerrain].target), static_cast<unsigned>(m_gbuffers[kFamWater].target),
                      static_cast<unsigned>(m_gbuffers[kFamEntities].target), static_cast<unsigned>(m_gbuffers[kFamBlock].target),
                      static_cast<unsigned>(m_gbuffers[kFamSky].target), static_cast<unsigned>(m_gbuffers[kFamClouds].target),
                      static_cast<unsigned>(m_gbuffers[kFamParticles].target), static_cast<unsigned>(m_gbuffers[kFamShadow].target));
            Log::Info("[ShaderPipeline]   engine programs matched: entities=%zu block=%zu sky=%zu clouds=%zu particles=%zu",
                      m_familyEngineShaders[kFamEntities].size(), m_familyEngineShaders[kFamBlock].size(),
                      m_familyEngineShaders[kFamSky].size(), m_familyEngineShaders[kFamClouds].size(),
                      m_familyEngineShaders[kFamParticles].size());
            for (int i = 0; i < kColorBuffers; ++i) {
                if (m_color[i].main == INVALID_TEXTURE) continue;
                Log::Info("[ShaderPipeline]   colortex%d format %d", i, static_cast<int>(m_color[i].format));
            }
            for (const Program& p : m_passes) {
                std::string db;
                for (int b : p.drawBuffers) db += std::to_string(b) + " ";
                Log::Info("[ShaderPipeline]   pass %s -> [%s]%s", p.name.c_str(), db.c_str(), p.isFinal ? " (final)" : "");
            }
            for (const Program& p : m_deferred) {
                std::string db;
                for (int b : p.drawBuffers) db += std::to_string(b) + " ";
                Log::Info("[ShaderPipeline]   deferred %s -> [%s]", p.name.c_str(), db.c_str());
            }
            static const char* kFamNames[kFamilyCount] = { "terrain", "water", "entities", "block", "sky", "clouds", "particles", "shadow" };
            for (int fam = 0; fam < kFamilyCount; ++fam) {
                const Program& p = m_gbuffers[fam];
                if (p.shader == INVALID_SHADER) continue;
                std::string db;
                for (int b : p.drawBuffers) db += std::to_string(b) + " ";
                Log::Info("[ShaderPipeline]   gbuffers %s = %s -> [%s] alphaRef %.2f", kFamNames[fam], p.name.c_str(), db.c_str(), p.alphaRef);
            }
        }
        EnsureEntityMap();
        g_renderBackend->SetShaderOverrideMode(true, m_sceneTarget);
        // MC clears every colortex but the first to transparent black (or
        // the pack's colortexNClearColor), and leaves alone the ones the
        // pack declares colortexNClear = false.
        for (const ClearGroup& g : m_clearGroups) {
            if (g.target == INVALID_RENDER_TARGET) continue;
            g_renderBackend->BindRenderTarget(g.target);
            g_renderBackend->SetClearColor(g.color.r, g.color.g, g.color.b, g.color.a);
            g_renderBackend->Clear(true, false, false);
        }
        BindInputs(Layout::Gbuffers);
        // The frame loop's own clear (fog colour, depth, stencil) lands here.
        g_renderBackend->BindRenderTarget(m_sceneTarget);
        m_inScene = true;
    }

    void ShaderPipeline::RenderShadowPass(const ShaderFrameInput& in) {
        m_haveShadow = false;
        const Program& shadow = m_gbuffers[kFamShadow];
        if (shadow.shader == INVALID_SHADER || m_shadowTarget == INVALID_RENDER_TARGET || !g_chunkRenderer) return;

        // The light's view: from 100 blocks toward the sun, looking at the
        // camera, world +Z up so the map's orientation never flips as the
        // sun passes overhead. The uniform form has the camera at the
        // origin, as MC's; the pass form is the same rotation in render
        // space. Orthographic over the pack's shadowDistance.
        const glm::vec3 sunDir = SunDirection(in.sunAngleDeg, m_sunPathRotation);
        const glm::vec3 lightDir = SunAngle(in.sunAngleDeg) < 0.5f ? sunDir : -sunDir;
        const glm::vec3 up(0.0f, 0.0f, 1.0f);
        m_shadowView = glm::lookAt(lightDir * 100.0f, glm::vec3(0.0f), up);
        const float d = m_shadowDistance;
        m_shadowProjection = glm::ortho(-d, d, -d, d, 0.05f, 256.0f);

        Camera cam;
        cam.position     = in.cameraPosition;
        cam.renderOrigin = Render::RenderOriginFor(cam.position);
        const glm::vec3 eye = cam.RenderPosition();
        cam.hasViewOverride = true;
        cam.viewOverride    = glm::lookAt(eye + lightDir * 100.0f, eye, up);
        cam.viewTilt        = glm::mat4(1.0f);
        const Frustum frustum = Frustum::FromMatrix(m_shadowProjection * cam.GetWorldViewMatrix());

        // The shadow program's frame uniforms: the light's matrices as its
        // model-view and projection.
        ShaderFrameInput sin = in;
        sin.view = cam.viewOverride;
        sin.projection = m_shadowProjection;
        g_renderBackend->BindShader(shadow.shader);
        SetUniforms(shadow, sin, Layout::Gbuffers);

        m_inShadowPass = true;
        InstallOverrides();
        g_renderBackend->BindRenderTarget(m_shadowTarget);
        g_renderBackend->SetClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        g_renderBackend->Clear(true, true, true);
        // A directional view (ChunkRenderer::SetDirectionalView): every
        // section of the light's box around the player, casters behind the
        // player included, face groups by the light's direction.
        g_chunkRenderer->SetDirectionalView(lightDir, in.cameraPosition);
        g_chunkRenderer->RenderAll(cam, frustum, m_shadowProjection, /*exactProjection=*/true);
        g_chunkRenderer->ClearDirectionalViewKeepSections();
        g_renderBackend->CheckErrors("ShaderPipeline.ShadowPass");
        m_inShadowPass = false;
        InstallOverrides();
        m_haveShadow = true;
        // The shadow maps on their units, now that they hold this frame.
        BindInputs(Layout::Gbuffers);
        g_renderBackend->BindRenderTarget(m_sceneTarget);
    }

    void ShaderPipeline::SetFrameInput(const ShaderFrameInput& in) {
        m_frame = in;
        if (!m_inScene || !g_renderBackend) return;
        // eyeBrightnessSmooth: MC eases eyeBrightness with a 0.5 s half-life.
        const glm::vec2 eye(static_cast<float>(std::clamp(in.eyeBlockLight, 0, 15) * 16),
                            static_cast<float>(std::clamp(in.eyeSkyLight, 0, 15) * 16));
        if (!m_haveEyeSmooth) { m_eyeBrightnessSmooth = eye; m_haveEyeSmooth = true; }
        else {
            const float k = 1.0f - std::pow(0.5f, std::max(0.0f, in.deltaSeconds) / 0.5f);
            m_eyeBrightnessSmooth += (eye - m_eyeBrightnessSmooth) * k;
        }
        EvaluateCustomUniforms(in);
        UpdateLightmap(in.skyBrightness);
        RenderShadowPass(in);
        for (int fam = 0; fam < kFamilyCount; ++fam) {
            const Program& p = m_gbuffers[fam];
            if (p.shader == INVALID_SHADER || !p.ownsShader || fam == kFamShadow) continue;
            g_renderBackend->BindShader(p.shader);
            SetUniforms(p, in, Layout::Gbuffers);
        }
        g_renderBackend->BindRenderTarget(m_sceneTarget);
    }

    // The average of a 16x16 patch at the centre of the bound target's first
    // attachment, logged against the frame's clear colour: says whether
    // anything was drawn there. Diagnostics only (frames 30..33).
    void ShaderPipeline::ProbeCentre(const char* what) {
        RenderBackend& b = *g_renderBackend;
        if (!b.RequestBackbufferReadback(m_width / 2 - 8, m_height / 2 - 8, 16, 16)) return;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!b.TakeBackbufferReadback(px, w, h) || px.size() < 4) return;
        unsigned long r = 0, g = 0, bl = 0;
        for (size_t i = 0; i + 3 < px.size(); i += 4) { r += px[i]; g += px[i + 1]; bl += px[i + 2]; }
        const size_t n = px.size() / 4;
        Log::Info("[ShaderPipeline] probe %s: avg (%lu, %lu, %lu) vs clear (%d, %d, %d)", what,
                  r / n, g / n, bl / n, static_cast<int>(m_frame.fogColor.r * 255),
                  static_cast<int>(m_frame.fogColor.g * 255), static_cast<int>(m_frame.fogColor.b * 255));
    }

    // Three points down the screen (top, centre, low), colour of the bound
    // target's first attachment and the live depth, in one line.
    void ShaderPipeline::ProbeThree(const char* what) {
        RenderBackend& b = *g_renderBackend;
        const int ys[3] = { m_height * 9 / 10, m_height / 2, m_height / 10 };
        std::string line = std::string("[ShaderPipeline] probe3 ") + what + ":";
        for (int i = 0; i < 3; ++i) {
            std::vector<uint8_t> px;
            int w = 0, hh = 0;
            unsigned long r = 0, g = 0, bl = 0;
            size_t n = 1;
            if (b.RequestBackbufferReadback(m_width / 2 - 4, ys[i] - 4, 8, 8) && b.TakeBackbufferReadback(px, w, hh) && px.size() >= 4) {
                for (size_t k = 0; k + 3 < px.size(); k += 4) { r += px[k]; g += px[k + 1]; bl += px[k + 2]; }
                n = px.size() / 4;
            }
            float d = -1.0f;
            b.ReadDepthPixel(m_sceneTarget, m_width / 2, ys[i], d);
            char buf[96];
            std::snprintf(buf, sizeof(buf), " [%s (%lu,%lu,%lu) depth %.5f]", i == 0 ? "top" : i == 1 ? "centre" : "low", r / n, g / n, bl / n, d);
            line += buf;
        }
        Log::Info("%s", line.c_str());
    }

    // The terrain's encoded view-space normals at three points, decoded the
    // way packs decode them (Lambert azimuthal), beside the view-space up
    // and sun vectors the pack lights with. A top face must decode close to
    // "up" and its NdotL close to dot(up, sun).
    void ShaderPipeline::ProbeNormals(const ShaderFrameInput& in) {
        if (m_color[2].main == INVALID_TEXTURE) return;
        RenderBackend& b = *g_renderBackend;
        const RenderTargetHandle rt = b.CreateRenderTargetFromTextures(&m_color[2].main, 1, INVALID_TEXTURE);
        if (rt == INVALID_RENDER_TARGET) return;
        b.BindRenderTarget(rt);
        const glm::mat3 rot(in.view);
        const glm::vec3 up = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 sun = glm::normalize(rot * SunDirection(in.sunAngleDeg, m_sunPathRotation));
        char line[512];
        std::snprintf(line, sizeof(line), "[ShaderPipeline] normals: view up (%.2f,%.2f,%.2f) sun (%.2f,%.2f,%.2f) SdotU %.2f sunAngleDeg %.1f dayTime %lld |",
                      up.x, up.y, up.z, sun.x, sun.y, sun.z, glm::dot(up, sun), in.sunAngleDeg, in.dayTime);
        std::string out = line;
        const int ys[3] = { m_height * 9 / 10, m_height / 2, m_height / 10 };
        for (int i = 0; i < 3; ++i) {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!b.RequestBackbufferReadback(m_width / 2, ys[i], 1, 1) || !b.TakeBackbufferReadback(px, w, h) || px.size() < 4) continue;
            const glm::vec2 enc(px[0] / 255.0f, px[1] / 255.0f);
            const glm::vec2 fenc = enc * 4.0f - 2.0f;
            const float f = glm::dot(fenc, fenc);
            const float g = std::sqrt(std::max(0.0f, 1.0f - f / 4.0f));
            const glm::vec3 n(fenc * g, 1.0f - f / 2.0f);
            std::snprintf(line, sizeof(line), " [%s n (%.2f,%.2f,%.2f) NdotU %.2f NdotL %.2f]",
                          i == 0 ? "top" : i == 1 ? "centre" : "low", n.x, n.y, n.z, glm::dot(n, up), glm::dot(n, sun));
            out += line;
        }
        Log::Info("%s", out.c_str());
        b.DestroyRenderTarget(rt);
    }

    void ShaderPipeline::ProbeTexture(TextureHandle tex, const char* what) {
        if (tex == INVALID_TEXTURE) return;
        RenderBackend& b = *g_renderBackend;
        const RenderTargetHandle rt = b.CreateRenderTargetFromTextures(&tex, 1, INVALID_TEXTURE);
        if (rt == INVALID_RENDER_TARGET) return;
        b.BindRenderTarget(rt);
        ProbeThree(what);
        b.DestroyRenderTarget(rt);
    }

    void ShaderPipeline::RunDeferred() {
        if (m_deferred.empty() || !g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;
        b.SetShaderOverrideMode(false, INVALID_RENDER_TARGET);
        RunPasses(m_deferred, m_frame);
        // The passes flipped buffers: the gbuffer targets follow the mains.
        RebuildGbufferTargets();
        InstallOverrides();
        b.SetShaderOverrideMode(true, m_sceneTarget);
        BindInputs(Layout::Gbuffers);
        PipelineState def;
        b.SetPipelineState(def);
        m_deferredRanThisFrame = true;
    }

    void ShaderPipeline::BindInputs(Layout layout) {
        RenderBackend& b = *g_renderBackend;
        auto colorOr = [&](int i) { return m_color[i].main != INVALID_TEXTURE ? m_color[i].main : m_white; };
        const TextureHandle shadow0 = m_haveShadow ? m_shadowDepth0 : m_whiteDepth;
        const TextureHandle shadow1 = m_haveShadow ? m_shadowDepth1 : m_whiteDepth;
        const TextureHandle shadowColor = m_haveShadow ? m_shadowColor0 : m_white;
        if (layout == Layout::Composite) {
            for (int i = 0; i < kColorBuffers; ++i) b.BindTexture(colorOr(i), static_cast<uint32_t>(i));
            b.BindTexture(m_depth, kCompDepth0);
            b.BindTexture(m_noise, kCompNoise);
            b.BindTexture(shadow0, kCompShadow0);
            b.BindTexture(shadowColor, kCompShadowColor);
            b.BindTexture(m_depth1, kCompDepth1);
            b.BindTexture(m_depth2, kCompDepth2);
            b.BindTexture(m_lightmap, kCompLightmap);
            b.BindTexture(shadow1, kCompShadow1);
        } else {
            b.BindTexture(m_lightmap, kGbufLightmap);
            b.BindTexture(m_flatNormal, kGbufNormals);
            b.BindTexture(m_white, kGbufSpecular);
            b.BindTexture(m_noise, kGbufNoise);
            b.BindTexture(m_depth, kGbufDepth0);
            b.BindTexture(m_depth1, kGbufDepth1);
            b.BindTexture(shadow0, kGbufShadow0);
            b.BindTexture(shadowColor, kGbufShadowColor);
            b.BindTexture(shadow1, kGbufShadow1);
            b.BindTexture(m_entityMap != INVALID_TEXTURE ? m_entityMap : m_white, kGbufEntityMap);
        }
    }

    void ShaderPipeline::SetSamplerUniforms(const Program& p, Layout layout) {
        RenderBackend& b = *g_renderBackend;
        const ShaderHandle s = p.shader;
        static const char* kColorNames[kColorBuffers]  = { "colortex0", "colortex1", "colortex2", "colortex3", "colortex4", "colortex5", "colortex6", "colortex7" };
        static const char* kLegacyNames[kColorBuffers] = { "gcolor", "gdepth", "gnormal", "composite", "gaux1", "gaux2", "gaux3", "gaux4" };
        if (layout == Layout::Composite) {
            for (int i = 0; i < kColorBuffers; ++i) {
                b.SetUniformInt(s, kColorNames[i], i);
                b.SetUniformInt(s, kLegacyNames[i], i);
            }
            b.SetUniformInt(s, "depthtex0", kCompDepth0);
            b.SetUniformInt(s, "gdepthtex", kCompDepth0);
            b.SetUniformInt(s, "depthtex1", kCompDepth1);
            b.SetUniformInt(s, "depthtex2", kCompDepth2);
            b.SetUniformInt(s, "noisetex", kCompNoise);
            b.SetUniformInt(s, "shadowtex0", kCompShadow0);
            b.SetUniformInt(s, "shadow", kCompShadow0);
            b.SetUniformInt(s, "shadowtex1", kCompShadow1);
            b.SetUniformInt(s, "watershadow", kCompShadow1);
            b.SetUniformInt(s, "shadowcolor", kCompShadowColor);
            b.SetUniformInt(s, "shadowcolor0", kCompShadowColor);
            b.SetUniformInt(s, "shadowcolor1", kCompShadowColor);
            b.SetUniformInt(s, "lightmap", kCompLightmap);
            b.SetUniformInt(s, "normals", kCompShadowColor);
            b.SetUniformInt(s, "specular", kCompShadowColor);
        } else {
            b.SetUniformInt(s, "gtexture", 0);
            b.SetUniformInt(s, "tex", 0);
            b.SetUniformInt(s, "lightmap", kGbufLightmap);
            b.SetUniformInt(s, "normals", kGbufNormals);
            b.SetUniformInt(s, "specular", kGbufSpecular);
            b.SetUniformInt(s, "noisetex", kGbufNoise);
            b.SetUniformInt(s, "depthtex0", kGbufDepth0);
            b.SetUniformInt(s, "gdepthtex", kGbufDepth0);
            b.SetUniformInt(s, "depthtex1", kGbufDepth1);
            b.SetUniformInt(s, "depthtex2", kGbufDepth1);
            b.SetUniformInt(s, "shadowtex0", kGbufShadow0);
            b.SetUniformInt(s, "shadow", kGbufShadow0);
            b.SetUniformInt(s, "shadowtex1", kGbufShadow1);
            b.SetUniformInt(s, "watershadow", kGbufShadow1);
            b.SetUniformInt(s, "shadowcolor", kGbufShadowColor);
            b.SetUniformInt(s, "shadowcolor0", kGbufShadowColor);
            b.SetUniformInt(s, "shadowcolor1", kGbufShadowColor);
        }
    }

    void ShaderPipeline::SetUniforms(const Program& p, const ShaderFrameInput& in, Layout layout) {
        RenderBackend& b = *g_renderBackend;
        const ShaderHandle s = p.shader;

        const glm::mat4 identity(1.0f);
        if (layout == Layout::Composite) {
            // The quad's own transform (see EnsureQuad).
            const glm::mat4 ortho = glm::ortho(0.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f);
            b.SetUniformMat4(s, "sp_ModelViewMatrix", identity);
            b.SetUniformMat4(s, "sp_ProjectionMatrix", ortho);
            b.SetUniformMat4(s, "sp_ModelViewProjectionMatrix", ortho);
            b.SetUniformMat4(s, "sp_ModelViewMatrixInverse", identity);
            b.SetUniformMat4(s, "sp_ProjectionMatrixInverse", glm::inverse(ortho));
            b.SetUniformMat4(s, "sp_TextureMatrix[0]", identity);
            b.SetUniformMat4(s, "sp_TextureMatrix[1]", identity);
        } else {
            // A gbuffers program transforms the engine's render-space vertex
            // with the frame's view and projection (uMVP, the product, is
            // the renderer's own upload).
            b.SetUniformMat4(s, "sp_ModelViewMatrix", in.view);
            b.SetUniformMat4(s, "sp_ProjectionMatrix", in.projection);
            b.SetUniformMat4(s, "sp_ModelViewMatrixInverse", glm::inverse(in.view));
            b.SetUniformMat4(s, "sp_ProjectionMatrixInverse", glm::inverse(in.projection));
            b.SetUniformMat4(s, "sp_TextureMatrix[0]", identity);
            // MC's lightmap matrix: 0..240 texel units to 1/32 .. 31/32.
            glm::mat4 lm(1.0f);
            lm = glm::translate(lm, glm::vec3(1.0f / 32.0f, 1.0f / 32.0f, 0.0f));
            lm = glm::scale(lm, glm::vec3(1.0f / 256.0f, 1.0f / 256.0f, 1.0f));
            b.SetUniformMat4(s, "sp_TextureMatrix[1]", lm);
            b.SetUniformVec3(s, "chunkOffset", glm::vec3(0.0f));
            b.SetUniformVec4(s, "entityColor", glm::vec4(0.0f));
            b.SetUniformInt(s, "entityId", 0);
            b.SetUniformInt(s, "blockEntityId", 0);
        }
        // The camera's rotation (the frame's, not the light's, so a shadow
        // program's gbuffer matrices are the camera's as in MC).
        const glm::mat3 rot(m_frame.view);
        b.SetUniformMat4(s, "sp_NormalMatrix4", glm::mat4(glm::transpose(glm::inverse(glm::mat3(in.view)))));

        // gbufferModelView is the view rotation with the camera at the
        // origin, as MC's; depth reconstruction through the projection
        // inverse is then camera-relative and cameraPosition completes it.
        const glm::mat4 modelView = glm::mat4(rot);
        const glm::mat4 modelViewInv = glm::inverse(modelView);
        const glm::mat4 projInv = glm::inverse(m_frame.projection);
        b.SetUniformMat4(s, "gbufferModelView", modelView);
        b.SetUniformMat4(s, "gbufferModelViewInverse", modelViewInv);
        b.SetUniformMat4(s, "gbufferProjection", m_frame.projection);
        b.SetUniformMat4(s, "gbufferProjectionInverse", projInv);
        b.SetUniformMat4(s, "gbufferPreviousModelView", m_havePrev ? m_prevModelView : modelView);
        b.SetUniformMat4(s, "gbufferPreviousProjection", m_havePrev ? m_prevProjection : m_frame.projection);
        const glm::mat4& sv = m_haveShadow || m_inShadowPass ? m_shadowView : identity;
        const glm::mat4& spj = m_haveShadow || m_inShadowPass ? m_shadowProjection : identity;
        b.SetUniformMat4(s, "shadowModelView", sv);
        b.SetUniformMat4(s, "shadowModelViewInverse", glm::inverse(sv));
        b.SetUniformMat4(s, "shadowProjection", spj);
        b.SetUniformMat4(s, "shadowProjectionInverse", glm::inverse(spj));

        const glm::vec3 cam(in.cameraPosition);
        b.SetUniformVec3(s, "cameraPosition", cam);
        b.SetUniformVec3(s, "previousCameraPosition", m_havePrev ? glm::vec3(m_prevCameraPosition) : cam);
        b.SetUniformFloat(s, "eyeAltitude", cam.y);

        // Celestial positions are view-space, 100 units out.
        const glm::vec3 sunDir = SunDirection(in.sunAngleDeg, m_sunPathRotation);
        const glm::vec3 sunPos = rot * (sunDir * 100.0f);
        const glm::vec3 moonPos = -sunPos;
        const float sunAngle = SunAngle(in.sunAngleDeg);
        b.SetUniformVec3(s, "sunPosition", sunPos);
        b.SetUniformVec3(s, "moonPosition", moonPos);
        b.SetUniformVec3(s, "shadowLightPosition", sunAngle < 0.5f ? sunPos : moonPos);
        b.SetUniformVec3(s, "upPosition", rot * glm::vec3(0.0f, 100.0f, 0.0f));
        b.SetUniformFloat(s, "sunAngle", sunAngle);
        b.SetUniformFloat(s, "shadowAngle", sunAngle < 0.5f ? sunAngle : sunAngle - 0.5f);
        b.SetUniformInt(s, "moonPhase", in.moonPhase);
        b.SetUniformInt(s, "worldTime", static_cast<int>(((in.dayTime % 24000) + 24000) % 24000));
        b.SetUniformInt(s, "worldDay", static_cast<int>(in.dayTime / 24000));

        b.SetUniformFloat(s, "viewWidth", static_cast<float>(m_width));
        b.SetUniformFloat(s, "viewHeight", static_cast<float>(m_height));
        b.SetUniformFloat(s, "aspectRatio", static_cast<float>(m_width) / static_cast<float>(std::max(1, m_height)));
        b.SetUniformFloat(s, "near", in.nearPlane);
        b.SetUniformFloat(s, "far", in.farPlane);
        b.SetUniformFloat(s, "frameTime", in.deltaSeconds);
        b.SetUniformFloat(s, "frameTimeCounter", m_frameTimeCounter);
        b.SetUniformInt(s, "frameCounter", m_frameCounter);

        b.SetUniformVec3(s, "skyColor", in.skyColor);
        b.SetUniformVec3(s, "fogColor", in.fogColor);
        b.SetUniformInt(s, "fogMode", 9729);   // GL_LINEAR
        b.SetUniformInt(s, "fogShape", 0);
        b.SetUniformFloat(s, "fogStart", in.fogStart);
        b.SetUniformFloat(s, "fogEnd", in.fogEnd);
        b.SetUniformFloat(s, "fogDensity", 0.0f);

        // Weather and effects the engine does not model yet.
        b.SetUniformFloat(s, "rainStrength", 0.0f);
        b.SetUniformFloat(s, "wetness", 0.0f);
        b.SetUniformFloat(s, "nightVision", 0.0f);
        b.SetUniformFloat(s, "blindness", 0.0f);
        b.SetUniformFloat(s, "darknessFactor", 0.0f);
        b.SetUniformFloat(s, "darknessLightFactor", 0.0f);
        b.SetUniformFloat(s, "screenBrightness", 1.0f);
        b.SetUniformFloat(s, "centerDepthSmooth", 1.0f);
        b.SetUniformInt(s, "isEyeInWater", in.isEyeInWater);
        b.SetUniformInt(s, "hideGUI", in.hideGui ? 1 : 0);
        b.SetUniformInt(s, "heldItemId", 0);
        b.SetUniformInt(s, "heldItemId2", 0);
        b.SetUniformInt(s, "heldBlockLightValue", 0);
        b.SetUniformInt(s, "heldBlockLightValue2", 0);
        // (block light, sky light) at the eye, 0..240 each; smoothed as MC does.
        b.SetUniformIVec2(s, "eyeBrightness", glm::ivec2(std::clamp(in.eyeBlockLight, 0, 15) * 16, std::clamp(in.eyeSkyLight, 0, 15) * 16));
        b.SetUniformIVec2(s, "eyeBrightnessSmooth", glm::ivec2(static_cast<int>(std::round(m_eyeBrightnessSmooth.x)),
                                                              static_cast<int>(std::round(m_eyeBrightnessSmooth.y))));
        b.SetUniformIVec2(s, "atlasSize", glm::ivec2(1024, 1024));

        // The pack's own uniforms (shaders.properties), this frame's values.
        for (const CustomUniform& u : m_customUniforms) {
            if (u.variable) continue;
            const float* v = u.value.v;
            if (u.type == "int" || u.type == "bool") b.SetUniformInt(s, u.name, static_cast<int>(v[0]));
            else if (u.type == "float") b.SetUniformFloat(s, u.name, v[0]);
            else if (u.type == "vec2") b.SetUniformVec2(s, u.name, glm::vec2(v[0], v[1]));
            else if (u.type == "vec3") b.SetUniformVec3(s, u.name, glm::vec3(v[0], v[1], v[2]));
            else if (u.type == "vec4") b.SetUniformVec4(s, u.name, glm::vec4(v[0], v[1], v[2], v[3]));
        }
    }

    void ShaderPipeline::DrawPass(const Program& p, const ShaderFrameInput& in) {
        RenderBackend& b = *g_renderBackend;
        if (p.isFinal) {
            b.BindRenderTarget(INVALID_RENDER_TARGET);
            b.SetViewport(0, 0, m_width, m_height);
            // The backbuffer's depth is the hand's and the HUD's; this frame
            // never cleared it (the level's clear went to the scene target).
            b.SetClearColor(in.fogColor.r, in.fogColor.g, in.fogColor.b, 1.0f);
            b.Clear(true, true, true);
        } else {
            // Each pass writes the ALT texture of every buffer it names and
            // the buffers flip afterwards, so the target is rebuilt per draw
            // from the current alt set (an FBO wrapping existing textures).
            std::vector<TextureHandle> outs;
            for (int idx : p.drawBuffers) outs.push_back(m_color[idx].alt);
            const RenderTargetHandle rt = b.CreateRenderTargetFromTextures(outs.data(), static_cast<int>(outs.size()), INVALID_TEXTURE);
            if (rt == INVALID_RENDER_TARGET) return;
            b.BindRenderTarget(rt);
            const_cast<Program&>(p).target = rt;
        }

        // shaders.properties `blend.<program>`: off unless the pack asks.
        PipelineState state;
        state.depthTestEnabled  = false;
        state.depthWriteEnabled = false;
        state.blendEnabled      = false;
        state.cullMode          = CullMode::None;
        auto bl = m_props.find("blend." + p.name);
        if (bl != m_props.end() && bl->second != "off") {
            std::stringstream ss(bl->second);
            std::string srcName, dstName;
            BlendFactor src, dst;
            if (ss >> srcName >> dstName && BlendFactorFrom(srcName, src) && BlendFactorFrom(dstName, dst)) {
                state.blendEnabled = true;
                state.srcBlendFactor = src;
                state.dstBlendFactor = dst;
            }
        }
        b.SetPipelineState(state);
        b.BindShader(p.shader);
        BindInputs(Layout::Composite);
        SetUniforms(p, in, Layout::Composite);
        if (p.isFinal && m_frameCounter % 300 >= 30 && m_frameCounter % 300 <= 31) {
            Log::Info("[ShaderPipeline] GL state before final draw: %s", b.DebugStateSummary().c_str());
        }
        b.DrawIndexed(m_quad, 6, 0);
        b.CheckErrors(p.isFinal ? "ShaderPipeline.final" : "ShaderPipeline.pass");
        if (p.isFinal && m_frameCounter % 300 >= 30 && m_frameCounter % 300 <= 33) ProbeThree("backbuffer after final");

        if (!p.isFinal) {
            b.DestroyRenderTarget(p.target);
            const_cast<Program&>(p).target = INVALID_RENDER_TARGET;
            for (int idx : p.drawBuffers) std::swap(m_color[idx].main, m_color[idx].alt);
        }
    }

    void ShaderPipeline::RunPasses(std::vector<Program>& passes, const ShaderFrameInput& in) {
        const bool probe = m_frameCounter % 300 >= 30 && m_frameCounter % 300 <= 33;
        for (const Program& p : passes) {
            DrawPass(p, in);
            if (probe && !p.drawBuffers.empty() && !p.isFinal) {
                const int first = p.drawBuffers[0];
                ProbeTexture(m_color[first].main, (std::string("after ") + p.name + ", colortex" + std::to_string(first)).c_str());
            }
        }
    }

    void ShaderPipeline::EndScene(const ShaderFrameInput& in) {
        if (!m_inScene || !g_renderBackend) return;
        m_inScene = false;
        RenderBackend& b = *g_renderBackend;
        b.SetShaderOverrideMode(false, INVALID_RENDER_TARGET);

        b.CheckErrors("ShaderPipeline.EndScene.before");
        if (m_frameCounter % 300 >= 30 && m_frameCounter % 300 <= 33) {
            b.BindRenderTarget(m_sceneTarget);
            ProbeCentre(m_deferredRanThisFrame ? "after level, colortex0 (deferred ran)" : "after level, colortex0");
        }
        m_deferredRanThisFrame = false;
        // depthtex2: the level's depth before the hand.
        if (m_depth2Target != INVALID_RENDER_TARGET) b.BlitRenderTargetDepth(m_sceneTarget, m_depth2Target);
        b.CheckErrors("ShaderPipeline.EndScene.blit");

        bool hadFinal = false;
        if (!kDebugNoComposites) {
            RunPasses(m_passes, in);
            for (const Program& p : m_passes) hadFinal = hadFinal || p.isFinal;
        }
        // Diagnostic (frames 34..37): the engine's own blit of colortex4 onto
        // the backbuffer, through the same quad, after the pack's final.
        if (m_frameCounter % 300 >= 34 && m_frameCounter % 300 <= 37 && m_color[4].main != INVALID_TEXTURE) {
            b.BindRenderTarget(INVALID_RENDER_TARGET);
            b.SetViewport(0, 0, m_width, m_height);
            PipelineState state;
            state.depthTestEnabled = false;
            state.depthWriteEnabled = false;
            state.blendEnabled = false;
            state.cullMode = CullMode::None;
            b.SetPipelineState(state);
            b.BindShader(m_blit);
            b.BindTexture(m_color[4].main, 0);
            b.SetUniformInt(m_blit, "colortex0", 0);
            b.DrawIndexed(m_quad, 6, 0);
            b.CheckErrors("ShaderPipeline.blitTest");
            ProbeThree("backbuffer after engine blit of colortex4");
        }
        if (!hadFinal) {
            // No final program: show colortex0 as it stands.
            b.BindRenderTarget(INVALID_RENDER_TARGET);
            b.SetViewport(0, 0, m_width, m_height);
            b.SetClearColor(in.fogColor.r, in.fogColor.g, in.fogColor.b, 1.0f);
            b.Clear(true, true, true);
            PipelineState state;
            state.depthTestEnabled = false;
            state.depthWriteEnabled = false;
            state.blendEnabled = false;
            state.cullMode = CullMode::None;
            b.SetPipelineState(state);
            b.BindShader(m_blit);
            b.BindTexture(m_color[0].main, 0);
            b.SetUniformInt(m_blit, "colortex0", 0);
            b.DrawIndexed(m_quad, 6, 0);
        }

        // The composites flipped buffers: next frame's targets follow.
        RebuildGbufferTargets();
        // Restore the state the renderers after us expect.
        PipelineState def;
        b.SetPipelineState(def);
        for (int i = 0; i < 16; ++i) b.BindTexture(INVALID_TEXTURE, static_cast<uint32_t>(i));

        m_prevModelView = glm::mat4(glm::mat3(in.view));
        m_prevProjection = in.projection;
        m_prevCameraPosition = in.cameraPosition;
        m_havePrev = true;
        // Advanced once the whole frame is drawn: the gbuffers programs and
        // the composites of one frame see the same frameCounter and
        // frameTimeCounter (a pack's TAA jitter and noise depend on it).
        m_frameTimeCounter = std::fmod(m_frameTimeCounter + std::max(0.0f, in.deltaSeconds), 3600.0f);
        ++m_frameCounter;
    }

} // namespace Render
