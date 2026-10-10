// File: src/client/renderer/gui/BlockIcon.cpp
//
// See BlockIcon.hpp. The model rules are MC's block model JSON (parents,
// `#texture` references, element faces with uv / rotation / tintindex) and
// the item definition's tint sources; the drawing is minecraft.icns's look.
#include "BlockIcon.hpp"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <unordered_map>

namespace Render::BlockIcon {

    namespace {

        namespace fs = std::filesystem;
        using nlohmann::json;

        // The look, measured off minecraft.icns (see tools/make_game_icon.py).
        constexpr float kShadeTop   = 1.0f;
        constexpr float kShadeLeft  = 0.92f;
        constexpr float kShadeRight = 0.45f;
        constexpr float kRimThickness = 0.0085f;   // of the icon height
        constexpr float kRimLift      = 0.4f;      // toward white
        constexpr int   kSupersample  = 4;         // per axis

        // "minecraft:block/stone" / "block/stone" → "block/stone". Other
        // namespaces are not in the engine's assets.
        std::optional<std::string> StripNamespace(const std::string& ref) {
            const size_t colon = ref.find(':');
            if (colon == std::string::npos) return ref;
            if (ref.compare(0, colon, "minecraft") != 0) return std::nullopt;
            return ref.substr(colon + 1);
        }

        std::optional<json> ReadJson(const fs::path& path) {
            std::ifstream in(path);
            if (!in.is_open()) return std::nullopt;
            try {
                return json::parse(in);
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }

        std::string TitleCase(const std::string& id) {
            std::string out;
            bool upper = true;
            for (char c : id) {
                if (c == '_') { out += ' '; upper = true; continue; }
                out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                upper = false;
            }
            return out;
        }

        uint32_t Multiply(uint32_t argb, uint32_t tint) {
            auto ch = [](uint32_t c, int shift) { return (c >> shift) & 0xFFu; };
            uint32_t out = argb & 0xFF000000u;
            for (int shift : {16, 8, 0}) {
                out |= ((ch(argb, shift) * ch(tint, shift) + 127u) / 255u) << shift;
            }
            return out;
        }

        // A block model with its parents folded in: the merged texture map
        // and the nearest `elements`.
        struct ResolvedModel {
            std::map<std::string, std::string> textures;
            json elements;
            bool isBlock = false;   // the chain reaches block/block (the GUI's block transform)
        };

        // One load's caches: parsing a model chain or a texture once, however
        // many blocks share it (every cube_all reads block/cube_all).
        class Loader {
        public:
            explicit Loader(fs::path assetsDir) : m_assets(std::move(assetsDir)) {}

            std::optional<Model> Load(const std::string& rawId) {
                const std::optional<std::string> id = StripNamespace(rawId);
                if (!id || id->empty()) return std::nullopt;
                const std::optional<json> item = ReadJson(m_assets / "items" / (*id + ".json"));
                if (!item || !item->contains("model")) return std::nullopt;
                const json& itemModel = (*item)["model"];
                // A plain model only: select / condition / special (chests,
                // heads, beds) and the rest draw something else.
                if (itemModel.value("type", std::string()) != "minecraft:model") return std::nullopt;
                const std::optional<std::string> modelRef = StripNamespace(itemModel.value("model", std::string()));
                if (!modelRef || modelRef->rfind("block/", 0) != 0) return std::nullopt;

                const std::optional<ResolvedModel> resolved = Resolve(*modelRef, 0);
                if (!resolved || !resolved->isBlock || !resolved->elements.is_array() ||
                    resolved->elements.empty()) {
                    return std::nullopt;
                }

                Model model;
                model.id = *id;
                model.name = TitleCase(*id);
                for (const json& element : resolved->elements) {
                    if (!IsWholeCube(element)) return std::nullopt;
                    Element out;
                    const json& faces = element.value("faces", json::object());
                    if (!LoadFace(faces, "up", *resolved, itemModel, out.up) ||
                        !LoadFace(faces, "north", *resolved, itemModel, out.north) ||
                        !LoadFace(faces, "west", *resolved, itemModel, out.west)) {
                        return std::nullopt;
                    }
                    model.elements.push_back(std::move(out));
                }
                // Each shown face's base layer — the first element that has
                // it — must exist and be solid; later layers may be cut out.
                for (Face Element::*face : {&Element::up, &Element::north, &Element::west}) {
                    const Face* base = nullptr;
                    for (const Element& e : model.elements) {
                        if ((e.*face).present) { base = &(e.*face); break; }
                    }
                    if (!base || !IsOpaque(*base)) return std::nullopt;
                }
                return model;
            }

        private:
            static bool IsWholeCube(const json& element) {
                if (element.contains("rotation")) return false;
                auto is = [&](const char* key, float value) {
                    const json& v = element.value(key, json::array());
                    if (!v.is_array() || v.size() != 3) return false;
                    for (const json& c : v) {
                        if (!c.is_number() || c.get<float>() != value) return false;
                    }
                    return true;
                };
                return is("from", 0.0f) && is("to", 16.0f);
            }

            // False on a face that is there but cannot be drawn (missing
            // texture); an absent face leaves `out` empty and is fine.
            bool LoadFace(const json& faces, const char* name, const ResolvedModel& model,
                          const json& itemModel, Face& out) {
                if (!faces.contains(name)) return true;
                const json& face = faces[name];
                const std::optional<std::string> tex = ResolveTexture(model, face.value("texture", std::string()));
                if (!tex) return false;
                const Image* image = Texture(*tex);
                if (!image) return false;
                out.present = true;
                out.texture = *image;
                if (face.contains("uv") && face["uv"].is_array() && face["uv"].size() == 4) {
                    for (int i = 0; i < 4; ++i) out.uv[i] = face["uv"][i].get<float>();
                }
                out.rotation = ((face.value("rotation", 0) % 360) + 360) % 360;
                if (face.contains("tintindex")) out.tint = Tint(itemModel, face["tintindex"].get<int>());
                return true;
            }

            // MC ItemModel tints: the item definition's tint source at the
            // face's tintindex, as the inventory draws it.
            uint32_t Tint(const json& itemModel, int index) {
                const json& tints = itemModel.value("tints", json::array());
                if (index < 0 || !tints.is_array() || index >= static_cast<int>(tints.size())) return 0xFFFFFFFFu;
                const json& source = tints[index];
                const std::string type = source.value("type", std::string());
                if (type == "minecraft:constant") {
                    return static_cast<uint32_t>(source.value("value", -1)) | 0xFF000000u;
                }
                if (type == "minecraft:grass") {
                    return GrassColor(source.value("temperature", 0.5), source.value("downfall", 1.0));
                }
                // Dye, potion, map colour …: their default.
                if (source.contains("default") && source["default"].is_number_integer()) {
                    return static_cast<uint32_t>(source["default"].get<int>()) | 0xFF000000u;
                }
                return 0xFFFFFFFFu;
            }

            // MC GrassColor.get(temperature, downfall) over the colormap.
            uint32_t GrassColor(double temperature, double downfall) {
                const Image* map = Texture("colormap/grass");
                downfall *= temperature;
                const int x = static_cast<int>((1.0 - temperature) * 255.0);
                const int y = static_cast<int>((1.0 - downfall) * 255.0);
                if (!map || map->width != 256 || x < 0 || y < 0 || x > 255 || y > 255) return 0xFFFF00FFu;
                const uint8_t* p = map->rgba.data() + (static_cast<size_t>(y) * 256 + x) * 4u;
                return 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
            }

            static bool IsOpaque(const Face& face) {
                const Image& t = face.texture;
                const float u0 = std::min(face.uv[0], face.uv[2]), u1 = std::max(face.uv[0], face.uv[2]);
                const float v0 = std::min(face.uv[1], face.uv[3]), v1 = std::max(face.uv[1], face.uv[3]);
                const int x0 = static_cast<int>(std::floor(u0 / 16.0f * t.width));
                const int x1 = static_cast<int>(std::ceil(u1 / 16.0f * t.width));
                const int y0 = static_cast<int>(std::floor(v0 / 16.0f * t.height));
                const int y1 = static_cast<int>(std::ceil(v1 / 16.0f * t.height));
                for (int y = std::max(y0, 0); y < std::min(y1, t.height); ++y) {
                    for (int x = std::max(x0, 0); x < std::min(x1, t.width); ++x) {
                        if (t.rgba[(static_cast<size_t>(y) * t.width + x) * 4u + 3] != 255) return false;
                    }
                }
                return true;
            }

            std::optional<ResolvedModel> Resolve(const std::string& ref, int depth) {
                if (depth > 16) return std::nullopt;   // a parent cycle
                const json* model = ModelJson(ref);
                if (!model) return std::nullopt;
                ResolvedModel out;
                if (model->contains("parent")) {
                    const std::optional<std::string> parent = StripNamespace((*model)["parent"].get<std::string>());
                    if (!parent) return std::nullopt;
                    std::optional<ResolvedModel> base = Resolve(*parent, depth + 1);
                    if (!base) return std::nullopt;
                    out = std::move(*base);
                }
                if (ref == "block/block") out.isBlock = true;
                if (model->contains("textures") && (*model)["textures"].is_object()) {
                    for (const auto& [key, value] : (*model)["textures"].items()) {
                        if (value.is_string()) out.textures[key] = value.get<std::string>();
                    }
                }
                if (model->contains("elements")) out.elements = (*model)["elements"];
                return out;
            }

            // "#side" through the texture map to "block/grass_block_side".
            static std::optional<std::string> ResolveTexture(const ResolvedModel& model, std::string ref) {
                for (int i = 0; i < 16 && !ref.empty() && ref[0] == '#'; ++i) {
                    const auto it = model.textures.find(ref.substr(1));
                    if (it == model.textures.end()) return std::nullopt;
                    ref = it->second;
                }
                if (ref.empty() || ref[0] == '#') return std::nullopt;
                return StripNamespace(ref);
            }

            const json* ModelJson(const std::string& ref) {
                auto it = m_models.find(ref);
                if (it == m_models.end()) {
                    it = m_models.emplace(ref, ReadJson(m_assets / "models" / (ref + ".json"))).first;
                }
                return it->second ? &*it->second : nullptr;
            }

            // A sprite, frame 0 of an animated strip (frames stack downward).
            const Image* Texture(const std::string& ref) {
                auto it = m_textures.find(ref);
                if (it == m_textures.end()) {
                    std::optional<Image> image;
                    const std::string path = (m_assets / "textures" / (ref + ".png")).string();
                    int w = 0, h = 0, channels = 0;
                    if (unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4)) {
                        Image img;
                        img.width = w;
                        img.height = std::min(w, h);
                        img.rgba.assign(pixels, pixels + static_cast<size_t>(img.width) * img.height * 4u);
                        stbi_image_free(pixels);
                        image = std::move(img);
                    }
                    it = m_textures.emplace(ref, std::move(image)).first;
                }
                return it->second ? &*it->second : nullptr;
            }

            fs::path m_assets;
            std::unordered_map<std::string, std::optional<json>>  m_models;
            std::unordered_map<std::string, std::optional<Image>> m_textures;
        };

        std::vector<std::string> ItemIds(const fs::path& assetsDir) {
            std::vector<std::string> ids;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(assetsDir / "items", ec)) {
                if (entry.path().extension() == ".json") ids.push_back(entry.path().stem().string());
            }
            std::sort(ids.begin(), ids.end());   // directory order is not stable
            return ids;
        }

        // The face's texel at face coordinates (s, t) in [0, 1): s across as
        // seen from outside, t down. ARGB, tinted.
        uint32_t Sample(const Face& face, float s, float t) {
            float a = s, b = t;
            switch (face.rotation) {   // the texture turned clockwise on the face
                case 90:  a = t;        b = 1.0f - s; break;
                case 180: a = 1.0f - s; b = 1.0f - t; break;
                case 270: a = 1.0f - t; b = s;        break;
                default: break;
            }
            const float u = face.uv[0] + a * (face.uv[2] - face.uv[0]);
            const float v = face.uv[1] + b * (face.uv[3] - face.uv[1]);
            const Image& tex = face.texture;
            const int x = std::clamp(static_cast<int>(u / 16.0f * tex.width), 0, tex.width - 1);
            const int y = std::clamp(static_cast<int>(v / 16.0f * tex.height), 0, tex.height - 1);
            const uint8_t* p = tex.rgba.data() + (static_cast<size_t>(y) * tex.width + x) * 4u;
            const uint32_t argb = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            return Multiply(argb, face.tint);
        }

        // Straight-alpha "over" of `top` onto `under`, both ARGB.
        uint32_t Over(uint32_t under, uint32_t top) {
            const float ta = static_cast<float>(top >> 24) / 255.0f;
            const float ua = static_cast<float>(under >> 24) / 255.0f;
            const float oa = ta + ua * (1.0f - ta);
            if (oa <= 0.0f) return 0;
            uint32_t out = static_cast<uint32_t>(std::lround(oa * 255.0f)) << 24;
            for (int shift : {16, 8, 0}) {
                const float tc = static_cast<float>((top >> shift) & 0xFFu);
                const float uc = static_cast<float>((under >> shift) & 0xFFu);
                const float c = (tc * ta + uc * ua * (1.0f - ta)) / oa;
                out |= static_cast<uint32_t>(std::lround(c)) << shift;
            }
            return out;
        }

    } // namespace

    std::optional<Model> LoadFullBlock(const std::filesystem::path& assetsDir, const std::string& id) {
        return Loader(assetsDir).Load(id);
    }

    std::vector<Model> LoadAllFullBlocks(const std::filesystem::path& assetsDir) {
        Loader loader(assetsDir);
        std::vector<Model> models;
        for (const std::string& id : ItemIds(assetsDir)) {
            if (std::optional<Model> m = loader.Load(id)) models.push_back(std::move(*m));
        }
        // The en_us names: block.minecraft.<id>, else item.minecraft.<id>.
        if (const std::optional<json> lang = ReadJson(assetsDir / "lang" / "en_us.json"); lang && lang->is_object()) {
            for (Model& m : models) {
                for (const char* prefix : {"block.minecraft.", "item.minecraft."}) {
                    const auto it = lang->find(prefix + m.id);
                    if (it != lang->end() && it->is_string()) { m.name = it->get<std::string>(); break; }
                }
            }
        }
        std::sort(models.begin(), models.end(),
                  [](const Model& a, const Model& b) { return a.name < b.name; });
        return models;
    }

    std::optional<Model> LoadRandomFullBlock(const std::filesystem::path& assetsDir, uint64_t seed) {
        std::vector<std::string> ids = ItemIds(assetsDir);
        std::mt19937_64 rng(seed);
        std::shuffle(ids.begin(), ids.end(), rng);
        Loader loader(assetsDir);
        for (const std::string& id : ids) {
            if (std::optional<Model> m = loader.Load(id)) return m;
        }
        return std::nullopt;
    }

    Image Render(const Model& model, int size) {
        Image out;
        if (size <= 0) return out;
        out.width = out.height = size;
        out.rgba.assign(static_cast<size_t>(size) * size * 4u, 0);

        // Geometry in supersampled pixels: the hexagon's full height less one
        // final pixel at each end (the anti-aliased tips would be cut by the
        // square). s is the projected edge length.
        const float big = static_cast<float>(size * kSupersample);
        const float s  = (big - 2.0f * kSupersample) / 2.0f;
        const float hw = s * std::sqrt(3.0f) / 2.0f;
        const float cx = big / 2.0f;
        const float y0 = static_cast<float>(kSupersample);
        const float rim = kRimThickness * big;
        const float rimScale = s * std::sin(2.0943951f);   // a top-face param → px from its front edge

        struct Quad { float ox, oy, ax, ay, bx, by, det; };
        auto quad = [](float ox, float oy, float ax, float ay, float bx, float by) {
            return Quad{ox, oy, ax, ay, bx, by, ax * by - ay * bx};
        };
        // Top: origin at the near (NW) corner, +a toward NE (left, texture
        // east), +b toward SW (right, texture south). North (left): from the
        // NE corner toward NW, then down. West (right): from NW toward SW,
        // then down. See tools/make_game_icon.py.
        const Quad top   = quad(cx, y0 + s, -hw, -s / 2.0f, hw, -s / 2.0f);
        const Quad left  = quad(cx - hw, y0 + s / 2.0f, hw, s / 2.0f, 0.0f, s);
        const Quad right = quad(cx, y0 + s, hw, -s / 2.0f, 0.0f, s);
        auto inside = [](const Quad& q, float px, float py, float& a, float& b) {
            const float dx = px - q.ox, dy = py - q.oy;
            a = (dx * q.by - dy * q.bx) / q.det;
            b = (q.ax * dy - q.ay * dx) / q.det;
            return a >= 0.0f && a < 1.0f && b >= 0.0f && b < 1.0f;
        };
        auto layered = [&](Face Element::*face, float a, float b) {
            uint32_t c = 0;
            for (const Element& e : model.elements) {
                if ((e.*face).present) c = Over(c, Sample(e.*face, a, b));
            }
            return c;
        };
        auto shade = [](uint32_t argb, float k, float lift) {
            uint32_t outc = argb & 0xFF000000u;
            for (int shift : {16, 8, 0}) {
                float c = static_cast<float>((argb >> shift) & 0xFFu) * k;
                c += (255.0f - c) * lift;
                outc |= static_cast<uint32_t>(std::lround(std::clamp(c, 0.0f, 255.0f))) << shift;
            }
            return outc;
        };

        const float inv = 1.0f / static_cast<float>(kSupersample * kSupersample);
        for (int py = 0; py < size; ++py) {
            for (int px = 0; px < size; ++px) {
                float r = 0.0f, g = 0.0f, bl = 0.0f, al = 0.0f;   // premultiplied sums
                for (int sy = 0; sy < kSupersample; ++sy) {
                    for (int sx = 0; sx < kSupersample; ++sx) {
                        const float x = static_cast<float>(px * kSupersample + sx) + 0.5f;
                        const float y = static_cast<float>(py * kSupersample + sy) + 0.5f;
                        float a = 0.0f, b = 0.0f;
                        uint32_t c = 0;
                        if (inside(top, x, y, a, b)) {
                            const bool onRim = std::min(a, b) * rimScale < rim;
                            c = shade(layered(&Element::up, a, b), kShadeTop, onRim ? kRimLift : 0.0f);
                        } else if (inside(left, x, y, a, b)) {
                            c = shade(layered(&Element::north, a, b), kShadeLeft, 0.0f);
                        } else if (inside(right, x, y, a, b)) {
                            c = shade(layered(&Element::west, a, b), kShadeRight, 0.0f);
                        } else {
                            continue;
                        }
                        const float ca = static_cast<float>(c >> 24) / 255.0f;
                        r  += static_cast<float>((c >> 16) & 0xFFu) * ca;
                        g  += static_cast<float>((c >> 8) & 0xFFu) * ca;
                        bl += static_cast<float>(c & 0xFFu) * ca;
                        al += ca;
                    }
                }
                if (al <= 0.0f) continue;
                uint8_t* p = out.rgba.data() + (static_cast<size_t>(py) * size + px) * 4u;
                p[0] = static_cast<uint8_t>(std::lround(std::clamp(r / al, 0.0f, 255.0f)));
                p[1] = static_cast<uint8_t>(std::lround(std::clamp(g / al, 0.0f, 255.0f)));
                p[2] = static_cast<uint8_t>(std::lround(std::clamp(bl / al, 0.0f, 255.0f)));
                p[3] = static_cast<uint8_t>(std::lround(std::clamp(al * inv * 255.0f, 0.0f, 255.0f)));
            }
        }
        return out;
    }

} // namespace Render::BlockIcon
