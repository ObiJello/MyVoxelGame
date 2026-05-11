// File: src/common/entity/ItemModelLoader.cpp
#include "ItemModelLoader.hpp"
#include "../core/Log.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>

// Bundle-aware asset path resolver lives in PlatformMain. Forward-declared so this
// `common/entity` translation unit doesn't take a hard `platform/` include dep — same
// pattern used by InventoryScreen.cpp.
namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Game {

    namespace {
        // Resolve a model identifier (e.g. "minecraft:item/compass_07" or "item/compass_07")
        // to the local file slug (e.g. "compass_07").
        std::string SlugFromModelRef(const std::string& ref) {
            std::string s = ref;
            // Strip namespace prefix (minecraft:)
            auto colon = s.find(':');
            if (colon != std::string::npos) s = s.substr(colon + 1);
            // Strip category prefix — item/ OR block/. Item models reference textures
            // from either dir (e.g. doors use item/oak_door, torches use block/torch).
            // GuiGraphics::LoadItemTexture searches both, so we drop the prefix here.
            for (const std::string& p : {std::string("item/"), std::string("block/")}) {
                if (s.compare(0, p.size(), p) == 0) { s = s.substr(p.size()); break; }
            }
            return s;
        }

        // Same idea but for a texture identifier ("minecraft:item/compass_07" → "compass_07").
        std::string TextureNameFromRef(const std::string& ref) {
            return SlugFromModelRef(ref); // same parsing rules
        }

        // Resolve `assets/models/item/{slug}.json` against the bundle's asset root using
        // the same bundle-aware resolver the rest of the engine uses.
        std::string ResolvePath(const std::string& slug) {
            return PlatformMain::GetAssetPath("assets/models/item/" + slug + ".json");
        }

        // Read + parse a single JSON file. Returns empty json on failure.
        nlohmann::json ReadJson(const std::string& path) {
            std::ifstream f(path);
            if (!f.is_open()) return nlohmann::json();
            try {
                nlohmann::json j;
                f >> j;
                return j;
            } catch (const std::exception& e) {
                Log::Warning("[ItemModelLoader] JSON parse error in %s: %s", path.c_str(), e.what());
                return nlohmann::json();
            }
        }

        // Extract `textures.layer0` from a parsed model JSON, returning the bare texture slug.
        // Returns empty string if not present.
        std::string ExtractLayer0(const nlohmann::json& model) {
            if (!model.is_object()) return {};
            auto texIt = model.find("textures");
            if (texIt == model.end() || !texIt->is_object()) return {};
            auto layerIt = texIt->find("layer0");
            if (layerIt == texIt->end() || !layerIt->is_string()) return {};
            return TextureNameFromRef(layerIt->get<std::string>());
        }

        // Extract ALL `textures.layerN` entries (layer0, layer1, …). MC's flat
        // item models can stack up to 4 layers (vanilla uses 2 for leather
        // armor + dye, spawn eggs, potions, fireworks, etc.). Order is
        // preserved so caller can pair with index-aligned tint values.
        std::vector<std::string> ExtractAllLayers(const nlohmann::json& model) {
            std::vector<std::string> out;
            if (!model.is_object()) return out;
            auto texIt = model.find("textures");
            if (texIt == model.end() || !texIt->is_object()) return out;
            for (int i = 0; ; ++i) {
                std::string key = "layer" + std::to_string(i);
                auto it = texIt->find(key);
                if (it == texIt->end() || !it->is_string()) break;
                out.push_back(TextureNameFromRef(it->get<std::string>()));
            }
            return out;
        }
    } // namespace

    bool ItemModelLoader::LoadInto(Item& item, const std::string& slug) {
        const std::string path = ResolvePath(slug);
        if (!std::filesystem::exists(path)) {
            return false;
        }
        nlohmann::json root = ReadJson(path);
        if (root.is_null()) return false;

        // Base sprite — `textures.layer0`.
        std::string baseSprite = ExtractLayer0(root);
        if (!baseSprite.empty()) item.spriteName = baseSprite;

        // All layers — for multi-layer items (leather armor + dye, spawn eggs,
        // potions, fireworks). spriteName above stays as layer0 for back-compat
        // with single-layer render paths; the renderer prefers spriteLayers
        // when there's more than one entry.
        // (NOTE: auto-detection of `_overlay` companion textures lives in
        // applyClientItem in Item.cpp — it needs to see the items.json tints
        // to decide whether MC would have generated a TWO_LAYERED_ITEM. Wolf
        // armor without dye is a counter-example: overlay PNG exists but the
        // un-dyed branch is single-layer, so we must NOT auto-attach it.)
        item.spriteLayers = ExtractAllLayers(root);

        // Overrides → animated frames. Each override references another model file whose
        // `textures.layer0` is the actual texture for that frame.
        auto ovIt = root.find("overrides");
        if (ovIt != root.end() && ovIt->is_array() && !ovIt->empty()) {
            // Collect (predicate value, texture slug) pairs.
            struct Variant { float predicate; std::string sprite; };
            std::vector<Variant> variants;
            variants.reserve(ovIt->size() + 1);

            // The base layer0 acts as the implicit "predicate 0" frame.
            if (!baseSprite.empty()) {
                variants.push_back({0.0f, baseSprite});
            }

            for (const auto& ov : *ovIt) {
                if (!ov.is_object()) continue;
                // Predicate is a single-key object like `{"angle": 0.5}`. We use the value
                // directly — caller's frame-selector defines what semantic the value has.
                auto pIt = ov.find("predicate");
                if (pIt == ov.end() || !pIt->is_object() || pIt->empty()) continue;
                // Capture the predicate NAME on the first override we see — every
                // override in a vanilla MC item model uses the same predicate
                // (compass.json is all "angle", clock.json is all "time", etc.).
                // Item.predicateName feeds ItemRegistry's selector-wiring logic.
                if (item.predicateName.empty()) {
                    item.predicateName = pIt->begin().key();
                }
                float predicate = pIt->begin().value().get<float>();

                // Resolve the referenced model and pull its layer0.
                auto mIt = ov.find("model");
                if (mIt == ov.end() || !mIt->is_string()) continue;
                std::string subSlug = SlugFromModelRef(mIt->get<std::string>());
                if (subSlug.empty()) continue;
                std::string subPath = ResolvePath(subSlug);
                if (!std::filesystem::exists(subPath)) {
                    Log::Warning("[ItemModelLoader] override model '%s' not found at %s",
                                 subSlug.c_str(), subPath.c_str());
                    continue;
                }
                nlohmann::json subModel = ReadJson(subPath);
                std::string subTex = ExtractLayer0(subModel);
                if (subTex.empty()) continue;
                variants.push_back({predicate, subTex});
            }

            // Sort by predicate so frame index 0..N maps to predicate value 0.0..1.0.
            std::sort(variants.begin(), variants.end(),
                      [](const Variant& a, const Variant& b) { return a.predicate < b.predicate; });

            item.spriteFrames.clear();
            item.spriteFrames.reserve(variants.size());
            for (const auto& v : variants) item.spriteFrames.push_back(v.sprite);
        }

        Log::Info("[ItemModelLoader] %s: layer0='%s' frames=%zu",
                  slug.c_str(), item.spriteName.c_str(), item.spriteFrames.size());
        return true;
    }

} // namespace Game
