// File: src/client/renderer/particle/ParticleSprites.cpp
#include "ParticleSprites.hpp"

#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/particle/ParticleOptions.hpp"

#include "stb_image.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        std::string StripNamespace(std::string_view id) {
            const size_t colon = id.find(':');
            return std::string(colon == std::string_view::npos ? id : id.substr(colon + 1));
        }

        bool ReadFile(const std::string& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

        int NextPow2(int v) {
            int p = 1;
            while (p < v) p <<= 1;
            return p;
        }

    } // namespace

    bool ParticleSpriteAtlas::Load() {
        Destroy();
        if (!g_renderBackend) return false;

        // 1. The sprite sets: particles/<type>.json for every vanilla type
        //    (ParticleResources.loadParticleDescription — {"textures": [...]}).
        std::vector<std::string> spriteNames;
        std::unordered_map<std::string, bool> seen;
        std::vector<std::pair<std::string, std::vector<std::string>>> setDefs;
        for (const Game::ParticleTypes::Info& info : Game::ParticleTypes::All()) {
            const std::string_view name(info.name);
            if (name.rfind("minecraft:", 0) != 0) continue;
            const std::string type = StripNamespace(name);
            const std::string path = PlatformMain::GetAssetPath("assets/particles/" + type + ".json");
            std::string text;
            if (!ReadFile(path, text)) continue;   // no JSON: the type binds no sprites (MC: NoRender / model types)
            try {
                const nlohmann::json j = nlohmann::json::parse(text);
                if (!j.contains("textures") || !j["textures"].is_array()) continue;
                std::vector<std::string> textures;
                for (const auto& t : j["textures"]) {
                    if (!t.is_string()) continue;
                    std::string tex = StripNamespace(t.get<std::string>());
                    textures.push_back(tex);
                    if (!seen[tex]) { seen[tex] = true; spriteNames.push_back(tex); }
                }
                setDefs.emplace_back(type, std::move(textures));
            } catch (const std::exception& e) {
                Log::Warning("[ParticleSprites] bad particle description %s: %s", path.c_str(), e.what());
            }
        }
        // The engine's own kinds borrow vanilla textures by name (the glint
        // for the Hush motes); make sure every sprite exists even if no JSON
        // happened to name it.
        for (const char* extra : {"glint", "generic_0", "flame", "bubble_white"}) {
            if (!seen[extra]) { seen[extra] = true; spriteNames.emplace_back(extra); }
        }

        // 2. Load every sprite (and its animation, if it has one).
        std::vector<Image> images;
        images.reserve(spriteNames.size());
        stbi_set_flip_vertically_on_load(0);
        std::vector<std::string> missing;
        for (const std::string& name : spriteNames) {
            const std::string rel = "assets/textures/particle/" + name + ".png";
            const std::string path = PlatformMain::GetAssetPath(rel);
            int w = 0, h = 0, ch = 0;
            unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!data) { missing.push_back(name); continue; }
            Image img;
            img.name = name;
            img.width = w;
            img.height = h;
            img.pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            stbi_image_free(data);

            // An animated sprite: frames stacked vertically, square unless the
            // mcmeta says otherwise (vibration.png.mcmeta: frametime 1).
            std::string meta;
            if (ReadFile(PlatformMain::GetAssetPath(rel + ".mcmeta"), meta)) {
                try {
                    const nlohmann::json j = nlohmann::json::parse(meta);
                    if (j.contains("animation") && j["animation"].is_object()) {
                        const auto& anim = j["animation"];
                        int frameW = anim.value("width", w);
                        int frameH = anim.value("height", std::min(w, h));
                        if (frameW <= 0 || frameW > w) frameW = w;
                        if (frameH <= 0 || frameH > h) frameH = std::min(w, h);
                        const int frames = std::max(1, h / frameH);
                        img.height = frameH;
                        img.width = frameW;
                        img.frames = frames;
                        img.frameTime = std::max(1, anim.value("frametime", 1));
                        if (anim.contains("frames") && anim["frames"].is_array()) {
                            for (const auto& f : anim["frames"]) {
                                int index = -1;
                                if (f.is_number_integer()) index = f.get<int>();
                                else if (f.is_object()) index = f.value("index", -1);
                                if (index >= 0 && index < frames) img.frameOrder.push_back(index);
                            }
                        }
                        // Frames wider than the sheet are cropped to its width.
                        if (frameW != w) {
                            std::vector<uint8_t> cropped(static_cast<size_t>(frameW) * static_cast<size_t>(frameH * frames) * 4u);
                            for (int y = 0; y < frameH * frames; ++y) {
                                std::copy_n(img.pixels.data() + static_cast<size_t>(y) * w * 4u,
                                            static_cast<size_t>(frameW) * 4u,
                                            cropped.data() + static_cast<size_t>(y) * frameW * 4u);
                            }
                            img.pixels.swap(cropped);
                        }
                    }
                } catch (const std::exception& e) {
                    Log::Warning("[ParticleSprites] bad animation meta for %s: %s", name.c_str(), e.what());
                }
            }
            images.push_back(std::move(img));
        }
        if (!missing.empty()) {
            std::string list;
            for (const std::string& m : missing) list += (list.empty() ? "" : ",") + m;
            Log::Warning("[ParticleSprites] Missing particle sprites: %s", list.c_str());
        }
        if (images.empty()) return false;

        // 3. Stitch: every frame of every sprite is its own cell; tallest
        //    first into shelves of a power-of-two width.
        struct Cell { int image; int frame; int w; int h; int x = 0; int y = 0; };
        std::vector<Cell> cells;
        size_t area = 0;
        int widest = 0;
        for (int i = 0; i < static_cast<int>(images.size()); ++i) {
            for (int f = 0; f < images[i].frames; ++f) {
                cells.push_back({i, f, images[i].width, images[i].height});
                area += static_cast<size_t>(images[i].width) * static_cast<size_t>(images[i].height);
            }
            widest = std::max(widest, images[i].width);
        }
        std::vector<size_t> order(cells.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (cells[a].h != cells[b].h) return cells[a].h > cells[b].h;
            return cells[a].w > cells[b].w;
        });
        const int atlasW = std::max(NextPow2(widest),
                                    NextPow2(static_cast<int>(std::ceil(std::sqrt(static_cast<double>(area) * 1.25)))));
        int penX = 0, penY = 0, shelfH = 0;
        for (size_t idx : order) {
            Cell& c = cells[idx];
            if (penX + c.w > atlasW) { penX = 0; penY += shelfH; shelfH = 0; }
            c.x = penX;
            c.y = penY;
            penX += c.w;
            shelfH = std::max(shelfH, c.h);
        }
        const int atlasH = NextPow2(std::max(1, penY + shelfH));

        std::vector<uint8_t> atlas(static_cast<size_t>(atlasW) * static_cast<size_t>(atlasH) * 4u, 0);
        for (const Cell& c : cells) {
            const Image& img = images[static_cast<size_t>(c.image)];
            for (int y = 0; y < c.h; ++y) {
                const uint8_t* src = img.pixels.data() +
                    (static_cast<size_t>(c.frame) * c.h + static_cast<size_t>(y)) * static_cast<size_t>(c.w) * 4u;
                uint8_t* dst = atlas.data() +
                    (static_cast<size_t>(c.y + y) * static_cast<size_t>(atlasW) + static_cast<size_t>(c.x)) * 4u;
                std::copy_n(src, static_cast<size_t>(c.w) * 4u, dst);
            }
        }

        m_texture = g_renderBackend->CreateTexture2D(atlasW, atlasH, TextureFormat::RGBA8, atlas.data());
        if (m_texture == INVALID_TEXTURE) {
            Log::Warning("[ParticleSprites] failed to create the %dx%d particle atlas", atlasW, atlasH);
            return false;
        }
        g_renderBackend->SetTextureFilter(m_texture, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_texture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        // 4. Sprite records.
        const float invW = 1.0f / static_cast<float>(atlasW);
        const float invH = 1.0f / static_cast<float>(atlasH);
        auto rectOf = [&](const Cell& c) {
            Rect r;
            r.u0 = static_cast<float>(c.x) * invW;
            r.v0 = static_cast<float>(c.y) * invH;
            r.u1 = static_cast<float>(c.x + c.w) * invW;
            r.v1 = static_cast<float>(c.y + c.h) * invH;
            return r;
        };
        // cells are image-major, frame-minor.
        size_t cellCursor = 0;
        m_sprites.resize(images.size());
        for (size_t i = 0; i < images.size(); ++i) {
            const Image& img = images[i];
            Sprite& s = m_sprites[i];
            std::vector<Rect> frameRects;
            for (int f = 0; f < img.frames; ++f) frameRects.push_back(rectOf(cells[cellCursor++]));
            s.rect = frameRects.front();
            if (img.frames > 1 || !img.frameOrder.empty()) {
                s.firstFrame = static_cast<uint32_t>(m_frames.size());
                if (img.frameOrder.empty()) {
                    for (const Rect& r : frameRects) m_frames.push_back(r);
                    s.frameCount = static_cast<uint16_t>(frameRects.size());
                } else {
                    for (int f : img.frameOrder) m_frames.push_back(frameRects[static_cast<size_t>(f)]);
                    s.frameCount = static_cast<uint16_t>(img.frameOrder.size());
                }
                s.frameTime = static_cast<uint16_t>(img.frameTime);
                s.rect = m_frames[s.firstFrame];
            }
            for (size_t px = 3; px < img.pixels.size(); px += 4) {
                const uint8_t a = img.pixels[px];
                if (a != 0 && a != 255) { s.translucent = true; break; }
            }
            m_spriteByName[img.name] = static_cast<int>(i);
        }

        // 5. Sets. MC binds the missing sprite for an unresolvable name; a
        //    missing file here is simply left out (and logged above).
        for (auto& [type, textures] : setDefs) {
            std::vector<int> ids;
            for (const std::string& t : textures) {
                const auto it = m_spriteByName.find(t);
                if (it != m_spriteByName.end()) ids.push_back(it->second);
            }
            if (ids.empty()) continue;
            m_setByType[type] = static_cast<int>(m_sets.size());
            m_sets.push_back(std::move(ids));
        }
        Log::Info("[ParticleSprites] %zu sprites, %zu sets, %dx%d atlas",
                  m_sprites.size(), m_sets.size(), atlasW, atlasH);
        return true;
    }

    void ParticleSpriteAtlas::Destroy() {
        if (m_texture != INVALID_TEXTURE && g_renderBackend) {
            // A frame in flight may still sample the atlas (Vulkan).
            g_renderBackend->DeferredDestroyTexture(m_texture);
        }
        m_texture = INVALID_TEXTURE;
        m_sprites.clear();
        m_frames.clear();
        m_spriteByName.clear();
        m_sets.clear();
        m_setByType.clear();
    }

    int ParticleSpriteAtlas::SetIndex(std::string_view typeName) const {
        const auto it = m_setByType.find(StripNamespace(typeName));
        return it == m_setByType.end() ? -1 : it->second;
    }

    int ParticleSpriteAtlas::SetSize(int set) const {
        if (set < 0 || static_cast<size_t>(set) >= m_sets.size()) return 0;
        return static_cast<int>(m_sets[static_cast<size_t>(set)].size());
    }

    int ParticleSpriteAtlas::Get(int set, int age, int lifetime) const {
        const int n = SetSize(set);
        if (n == 0) return -1;
        const int max = std::max(lifetime, 1);
        const int index = std::clamp(age * (n - 1) / max, 0, n - 1);
        return m_sets[static_cast<size_t>(set)][static_cast<size_t>(index)];
    }

    int ParticleSpriteAtlas::Random(int set, Game::JavaRandom& random) const {
        const int n = SetSize(set);
        if (n == 0) return -1;
        return m_sets[static_cast<size_t>(set)][static_cast<size_t>(random.NextInt(n))];
    }

    int ParticleSpriteAtlas::First(int set) const {
        if (SetSize(set) == 0) return -1;
        return m_sets[static_cast<size_t>(set)].front();
    }

    int ParticleSpriteAtlas::SpriteIndex(std::string_view textureName) const {
        const auto it = m_spriteByName.find(StripNamespace(textureName));
        return it == m_spriteByName.end() ? -1 : it->second;
    }

    ParticleSpriteAtlas::Rect ParticleSpriteAtlas::FrameRect(int id, int64_t clientTicks) const {
        const Sprite& s = GetSprite(id);
        if (s.frameCount <= 1) return s.rect;
        const int64_t frame = (clientTicks / std::max<int64_t>(s.frameTime, 1)) % s.frameCount;
        return m_frames[s.firstFrame + static_cast<size_t>(frame)];
    }

} // namespace Render
