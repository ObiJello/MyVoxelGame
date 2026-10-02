// File: src/common/entity/ClientItemLoader.cpp
#include "ClientItemLoader.hpp"
#include "Item.hpp"
#include "../data/DataComponents.hpp"
#include "../core/Log.hpp"
#include "../world/biome/Biomes.hpp"   // grass/foliage tint sampling

#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Game {

    namespace {
        // "minecraft:block/oak_trapdoor_bottom" → "block/oak_trapdoor_bottom"
        // "item/torch"                          → "item/torch"
        std::string StripNamespace(const std::string& ref) {
            auto colon = ref.find(':');
            return (colon == std::string::npos) ? ref : ref.substr(colon + 1);
        }

        // "block/oak_trapdoor_bottom" → "oak_trapdoor_bottom"
        // "item/torch"                → "torch"
        std::string LeafSlug(const std::string& ref) {
            std::string s = StripNamespace(ref);
            for (const std::string& p : {std::string("block/"), std::string("item/")}) {
                if (s.compare(0, p.size(), p) == 0) return s.substr(p.size());
            }
            return s;
        }

        bool IsBlockRef(const std::string& ref) {
            std::string s = StripNamespace(ref);
            return s.compare(0, 6, "block/") == 0;
        }

        // Recursively resolve a node to a single rest model ref + (if encountered) the
        // most-specific range_dispatch's entries as animation frames. We always take the
        // "default" path (fallback / on_false / first case) so the result describes the
        // resting visual when no special game state is active.
        //
        // NOTE: only the FIRST range_dispatch we hit becomes the animation. Items
        // with nested dispatches (compass-in-end-dimension etc.) collapse to a
        // single animated frame set — fidelity loss here is acceptable for v1.
        // MC com.mojang.math.Transformation's codec: translation (blocks),
        // left_rotation / right_rotation (quaternions [x,y,z,w], or
        // {axis, angle}), scale. A BlockModel can only carry an axis-aligned
        // quarter turn and a translation, so the composed rotation is snapped
        // to one and anything else is refused (the caller then falls back).
        bool ReadQuaternion(const nlohmann::json& node, const char* key, glm::quat& out) {
            out = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            auto it = node.find(key);
            if (it == node.end()) return true;
            if (it->is_array() && it->size() == 4) {
                // Stored [x, y, z, w]; glm::quat takes (w, x, y, z).
                out = glm::normalize(glm::quat((*it)[3].get<float>(), (*it)[0].get<float>(),
                                               (*it)[1].get<float>(), (*it)[2].get<float>()));
                return true;
            }
            if (it->is_object() && it->contains("axis") && it->contains("angle")) {
                const auto& a = (*it)["axis"];
                if (!a.is_array() || a.size() != 3) return false;
                const glm::vec3 axis(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
                out = glm::angleAxis(glm::radians((*it)["angle"].get<float>()), glm::normalize(axis));
                return true;
            }
            return false;
        }

        bool DecodeTransformation(const nlohmann::json& child, CompositeChild& out) {
            auto t = child.find("transformation");
            if (t == child.end()) return true;   // identity
            if (!t->is_object()) return false;

            glm::quat left, right;
            if (!ReadQuaternion(*t, "left_rotation", left) ||
                !ReadQuaternion(*t, "right_rotation", right)) {
                return false;
            }
            if (auto s = t->find("scale"); s != t->end() && s->is_array() && s->size() == 3) {
                for (int i = 0; i < 3; ++i) {
                    if (std::abs((*s)[i].get<float>() - 1.0f) > 1e-3f) return false;   // no scale in a BlockModel
                }
            }
            glm::vec3 translation(0.0f);
            if (auto tr = t->find("translation"); tr != t->end() && tr->is_array() && tr->size() == 3) {
                translation = glm::vec3((*tr)[0].get<float>(), (*tr)[1].get<float>(), (*tr)[2].get<float>());
            }

            // Transformation = T · R_left · S · R_right, scale being 1 here.
            const glm::quat rotation = glm::normalize(left * right);

            // Snap to a whole quarter turn about X or Y. RotateModel's turn is
            // MC's blockstate `x`/`y` step — a −90° right-handed rotation per
            // turn — so a right-handed angle θ about +axis is −θ/90 turns.
            int xTurns = 0, yTurns = 0;
            const float w = glm::clamp(rotation.w, -1.0f, 1.0f);
            const float angleDeg = glm::degrees(2.0f * std::acos(w));
            if (angleDeg > 1.0f && angleDeg < 359.0f) {
                const float s = std::sqrt(std::max(0.0f, 1.0f - w * w));
                const glm::vec3 axis = glm::vec3(rotation.x, rotation.y, rotation.z) / s;
                const float quarters = angleDeg / 90.0f;
                const int q = static_cast<int>(std::lround(quarters));
                if (std::abs(quarters - static_cast<float>(q)) > 0.02f) return false;
                auto turns = [&](float component) {
                    const int signed_ = component < 0.0f ? -q : q;
                    return ((-signed_) % 4 + 4) % 4;
                };
                if (std::abs(axis.y) > 0.99f)      yTurns = turns(axis.y);
                else if (std::abs(axis.x) > 0.99f) xTurns = turns(axis.x);
                else return false;   // Z or a diagonal: not bakeable
            }

            // RotateModel pivots about the cell centre C = (8,8,8); MC rotates
            // about the origin and then translates. rotate-about-C followed
            // by an offset o equals rotate-about-origin followed by
            // o + (C − R·C), so o = t·16 − (C − R·C) = t·16 + R·C − C.
            const glm::vec3 centre(8.0f);
            const glm::vec3 rotatedCentre = rotation * centre;
            out.xQuarterTurns = xTurns;
            out.yQuarterTurns = yTurns;
            out.offsetPx      = translation * 16.0f + rotatedCentre - centre;
            // Snap the residue (float noise from the quaternion) to the pixel.
            for (int i = 0; i < 3; ++i) out.offsetPx[i] = std::round(out.offsetPx[i] * 16.0f) / 16.0f;
            return true;
        }

        // Any node (or tint source) of the tree that reads CUSTOM_MODEL_DATA.
        bool ReadsCmd(const nlohmann::json& node, int depth) {
            if (depth > 128) return false;
            if (node.is_object()) {
                for (const char* key : {"property", "type"}) {
                    auto p = node.find(key);
                    // custom_model_data, and the other properties a stack
                    // answers (SelectValue): trim_material.
                    if (p != node.end() && p->is_string() &&
                        (StripNamespace(p->get<std::string>()) == "custom_model_data" ||
                         StripNamespace(p->get<std::string>()) == "trim_material")) {
                        return true;
                    }
                }
                for (const auto& [k, v] : node.items()) {
                    (void)k;
                    if ((v.is_object() || v.is_array()) && ReadsCmd(v, depth + 1)) return true;
                }
            } else if (node.is_array()) {
                for (const auto& v : node) if (ReadsCmd(v, depth + 1)) return true;
            }
            return false;
        }

        void Resolve(const nlohmann::json& node, ClientItemDesc& out, bool& foundFrames) {
            if (!node.is_object()) return;
            auto typeIt = node.find("type");
            if (typeIt == node.end() || !typeIt->is_string()) return;
            const std::string type = StripNamespace(typeIt->get<std::string>());

            if (type == "model") {
                auto m = node.find("model");
                if (m != node.end() && m->is_string()) {
                    const std::string ref = m->get<std::string>();
                    out.restSlug = LeafSlug(ref);
                    out.kind = IsBlockRef(ref) ? ClientItemKind::BlockModel
                                               : ClientItemKind::FlatSprite;
                }
                // Capture per-layer tints. MC's "tints" array is index-aligned
                // with the model's layerN textures. Each entry is one of
                // several tint-source types ("minecraft:dye", "constant",
                // "custom_model_data", …) — for v1 we read the `default`
                // field if present, falling back to 0 (= no tint, render
                // white). Leather armor uses {"type":"dye","default":-6265536}.
                auto tints = node.find("tints");
                if (tints != node.end() && tints->is_array()) {
                    out.layerTints.reserve(tints->size());
                    out.layerTintKinds.reserve(tints->size());
                    for (const auto& t : *tints) {
                        // One kind per entry, pushed before the value so every
                        // `continue` below leaves the two arrays aligned.
                        out.layerTintKinds.push_back(ItemTintKind::Fixed);
                        out.layerTintIndices.push_back(0);
                        if (!t.is_object()) { out.layerTints.push_back(0); continue; }

                        // Climate-sampled tint sources. MC's `minecraft:grass`
                        // and `minecraft:foliage` carry a fixed temperature /
                        // downfall instead of a `default`, because an ITEM has
                        // no biome to sample — bush ships
                        // {"type":"minecraft:grass","temperature":0.5,
                        // "downfall":1.0}.
                        //
                        // These MUST be handled: plant sprites are authored
                        // greyscale and get all their colour from here, so
                        // falling through to the `default` branch below (and
                        // therefore to 0 = untinted) renders a bush as a grey
                        // smear.
                        auto ty = t.find("type");
                        if (ty != t.end() && ty->is_string()) {
                            std::string tt = ty->get<std::string>();
                            if (tt.rfind("minecraft:", 0) == 0) tt.erase(0, 10);
                            // The potion source's colour is the STACK's
                            // PotionContents; its `default` (read below) is
                            // only the fallback. Item::ResolveLayerTint.
                            if (tt == "potion") out.layerTintKinds.back() = ItemTintKind::Potion;
                            if (tt == "dye")    out.layerTintKinds.back() = ItemTintKind::Dye;
                            if (tt == "map_color") out.layerTintKinds.back() = ItemTintKind::MapColor;
                            if (tt == "firework")  out.layerTintKinds.back() = ItemTintKind::Firework;
                            if (tt == "custom_model_data") {
                                // CustomModelDataSource {index (NON_NEGATIVE_INT, 0),
                                // default (RGB_COLOR_CODEC: an int or [r, g, b])}.
                                out.layerTintKinds.back() = ItemTintKind::CustomModelData;
                                auto ix = t.find("index");
                                if (ix != t.end() && ix->is_number_integer()) {
                                    out.layerTintIndices.back() =
                                        static_cast<uint8_t>(std::clamp<int64_t>(ix->get<int64_t>(), 0, 255));
                                }
                                auto d = t.find("default");
                                if (d != t.end() && d->is_array() && d->size() == 3) {
                                    uint32_t rgb = 0;
                                    for (size_t c = 0; c < 3; ++c) {
                                        const float f = (*d)[c].is_number() ? (*d)[c].get<float>() : 0.0f;
                                        rgb = (rgb << 8) | static_cast<uint32_t>(
                                            std::clamp(static_cast<int>(std::floor(f * 255.0f + 0.5f)), 0, 255));
                                    }
                                    out.layerTints.push_back(rgb | 0xFF000000u);
                                    continue;
                                }
                            }
                            if (tt == "grass" || tt == "foliage") {
                                const auto num = [&](const char* k, float dflt) {
                                    auto f = t.find(k);
                                    return (f != t.end() && f->is_number())
                                         ? f->get<float>() : dflt;
                                };
                                const float temp = num("temperature", 0.5f);
                                const float down = num("downfall", 1.0f);
                                out.layerTints.push_back(
                                    tt == "grass"
                                        ? BiomeRegistry::GrassColorAt(temp, down)
                                        : BiomeRegistry::FoliageColorAt(temp, down));
                                continue;
                            }
                        }

                        // `minecraft:constant` carries its colour as `value`
                        // (ItemTintSources.CONSTANT); every other source's
                        // fallback is `default`. The engine-only spawn eggs
                        // are two constant tints over a shared template.
                        auto def = t.find("value");
                        if (def == t.end() || !def->is_number_integer()) def = t.find("default");
                        if (def != t.end() && def->is_number_integer()) {
                            // Java int → ARGB uint32 (negative ints store the
                            // high alpha bit set; reinterpreting two's-complement
                            // gives the canonical ARGB form, e.g. -6265536 →
                            // 0xFFA06540).
                            const int64_t raw = def->get<int64_t>();
                            out.layerTints.push_back(static_cast<uint32_t>(raw & 0xFFFFFFFFu));
                        } else {
                            out.layerTints.push_back(0);
                        }
                    }
                }
                return;
            }
            if (type == "special") {
                // BEWLR custom renderer (chest, sign, banner, shield, trident, …).
                // We render the `base` model as a flat sprite as a sane fallback —
                // matches MC's behaviour when the BEWLR isn't loaded.
                auto b = node.find("base");
                if (b != node.end() && b->is_string()) {
                    out.restSlug = LeafSlug(b->get<std::string>());
                }
                // Inner `model` describes which BEWLR + which texture variant.
                auto inner = node.find("model");
                if (inner != node.end() && inner->is_object()) {
                    auto innerType = inner->find("type");
                    if (innerType != inner->end() && innerType->is_string()) {
                        out.specialKind = StripNamespace(innerType->get<std::string>());
                    }
                    // Variant identifier — different special kinds use different keys:
                    //   chest/shulker_box → "texture" (e.g. "minecraft:trapped")
                    //   banner            → "color"   (e.g. "red")
                    //   head              → "kind"    (e.g. "creeper", "skeleton")
                    //   player_head       → none      (always uses default Steve skin)
                    auto innerTex = inner->find("texture");
                    if (innerTex != inner->end() && innerTex->is_string()) {
                        out.specialTexture = StripNamespace(innerTex->get<std::string>());
                    } else if (auto innerColor = inner->find("color");
                               innerColor != inner->end() && innerColor->is_string()) {
                        out.specialTexture = StripNamespace(innerColor->get<std::string>());
                    } else if (auto innerKind = inner->find("kind");
                               innerKind != inner->end() && innerKind->is_string()) {
                        out.specialTexture = StripNamespace(innerKind->get<std::string>());
                    }
                }
                out.kind = ClientItemKind::Special;
                return;
            }
            if (type == "condition") {
                // We don't simulate the boolean conditions (lodestone tracker, broken
                // tools, fishing cast, etc.) — always take on_false (resting state).
                auto f = node.find("on_false");
                if (f != node.end()) Resolve(*f, out, foundFrames);
                return;
            }
            if (type == "select") {
                // Discrete properties (chest local-time, dimension, charge
                // type, display_context, …). We resolve to the variant the
                // INVENTORY would see — that means:
                //   1. If the select's `property` is `display_context`,
                //      prefer the case whose `when` array contains "gui"
                //      (trident does this — its GUI icon is a flat sprite,
                //      its in-hand model is a BEWLR special). Without this
                //      preference, the fallback is the in-hand BEWLR which
                //      has no flat texture → invisible inventory icon.
                //   2. Otherwise (different property), use the `fallback`.
                auto prop = node.find("property");
                std::string propName = (prop != node.end() && prop->is_string())
                                       ? StripNamespace(prop->get<std::string>())
                                       : std::string();
                if (propName == "display_context") {
                    auto cases = node.find("cases");
                    if (cases != node.end() && cases->is_array()) {
                        for (const auto& c : *cases) {
                            if (!c.is_object()) continue;
                            auto when = c.find("when");
                            if (when == c.end()) continue;
                            // `when` may be a single string or an array of strings.
                            bool guiMatch = false;
                            if (when->is_string()) {
                                guiMatch = (when->get<std::string>() == "gui");
                            } else if (when->is_array()) {
                                for (const auto& w : *when) {
                                    if (w.is_string() && w.get<std::string>() == "gui") {
                                        guiMatch = true; break;
                                    }
                                }
                            }
                            if (guiMatch) {
                                auto m = c.find("model");
                                if (m != c.end()) Resolve(*m, out, foundFrames);
                                return;
                            }
                        }
                    }
                }
                auto fb = node.find("fallback");
                if (fb != node.end()) Resolve(*fb, out, foundFrames);
                return;
            }
            if (type == "range_dispatch") {
                // First range_dispatch we hit defines the animation frames.
                if (!foundFrames) {
                    auto entries = node.find("entries");
                    if (entries != node.end() && entries->is_array()) {
                        for (const auto& e : *entries) {
                            if (!e.is_object()) continue;
                            auto m = e.find("model");
                            if (m == e.end() || !m->is_object()) continue;
                            // Resolve the entry recursively in case it's another
                            // dispatch — but only capture its rest ref (no nested frames).
                            ClientItemDesc subRest;
                            bool subFrames = true; // already had ours; suppress further capture
                            Resolve(*m, subRest, subFrames);
                            if (!subRest.restSlug.empty()) {
                                out.frameSlugs.push_back(subRest.restSlug);
                            }
                        }
                        auto prop = node.find("property");
                        if (prop != node.end() && prop->is_string()) {
                            out.property = prop->get<std::string>();
                        }
                        foundFrames = true;
                    }
                }
                // Use fallback for resting model (frame 0 of the animation).
                auto fb = node.find("fallback");
                if (fb != node.end()) Resolve(*fb, out, foundFrames);
                // If the dispatch itself had no fallback (e.g. clock.json — just
                // entries + property), fall back to the FIRST entry as the
                // resting visual. Without this, items like clock end up with
                // populated frameSlugs but kind=Missing → applyClientItem
                // early-returns and the frames are thrown away.
                if (out.kind == ClientItemKind::Missing && !out.frameSlugs.empty()) {
                    out.restSlug = out.frameSlugs.front();
                    out.kind     = ClientItemKind::FlatSprite;
                }
                return;
            }
            if (type == "composite") {
                // MC CompositeModel: every child drawn with its own
                // transformation. Children that are block models with a
                // bakeable transformation become one synthetic BlockModel
                // (ItemRegistry::BakeCompositeItemModels); anything else
                // falls back to the first child's own look.
                auto models = node.find("models");
                if (models == node.end() || !models->is_array() || models->empty()) return;
                std::vector<CompositeChild> children;
                ClientItemDesc first;
                bool firstSet = false, bakeable = true;
                for (const auto& childNode : *models) {
                    ClientItemDesc sub;
                    bool subFrames = false;
                    Resolve(childNode, sub, subFrames);
                    if (!firstSet) { first = sub; firstSet = true; }
                    if (sub.kind != ClientItemKind::BlockModel) { bakeable = false; continue; }
                    CompositeChild child;
                    child.modelSlug = sub.restSlug;
                    if (!DecodeTransformation(childNode, child)) {
                        Log::Warning("[ClientItemLoader] composite child '%s' has a transformation a block model cannot carry",
                                     sub.restSlug.c_str());
                        bakeable = false;
                        continue;
                    }
                    children.push_back(std::move(child));
                }
                if (bakeable && !children.empty()) {
                    out.kind = ClientItemKind::Composite;
                    out.compositeChildren = std::move(children);
                    // A registry name no file could collide with; the bake
                    // registers the merged model under it.
                    std::string name = "composite";
                    for (const auto& c : out.compositeChildren) {
                        name += "/" + c.modelSlug + "@x" + std::to_string(c.xQuarterTurns) +
                                "y" + std::to_string(c.yQuarterTurns) +
                                "o" + std::to_string(static_cast<int>(std::lround(c.offsetPx.x))) +
                                "," + std::to_string(static_cast<int>(std::lround(c.offsetPx.y))) +
                                "," + std::to_string(static_cast<int>(std::lround(c.offsetPx.z)));
                    }
                    out.restSlug = name;
                } else if (firstSet) {
                    out = first;
                }
                return;
            }
            // Unknown type — leave kind=Missing.
        }
    } // namespace

    ClientItemDesc ClientItemLoader::Load(const std::string& slug) {
        ClientItemDesc desc;
        const std::string path = PlatformMain::GetAssetPath("assets/items/" + slug + ".json");
        if (!std::filesystem::exists(path)) return desc;

        std::ifstream f(path);
        if (!f.is_open()) return desc;

        nlohmann::json root;
        try { f >> root; }
        catch (const std::exception& e) {
            Log::Warning("[ClientItemLoader] %s parse error: %s", path.c_str(), e.what());
            return desc;
        }
        auto modelIt = root.find("model");
        if (modelIt == root.end()) return desc;
        bool foundFrames = false;
        Resolve(*modelIt, desc, foundFrames);
        desc.readsCustomModelData = ReadsCmd(*modelIt, 0);
        return desc;
    }

    namespace {

        // The parsed definitions, by id ("minecraft:stick"). A null entry
        // records a missing / unreadable file. Entries are never erased, so
        // the node pointers Evaluate hands out as leaf keys stay valid.
        struct DefinitionCache {
            std::mutex mutex;
            std::unordered_map<std::string, std::shared_ptr<const nlohmann::json>> byId;
        };
        DefinitionCache& Definitions() {
            static DefinitionCache cache;
            return cache;
        }

        std::string NormalizeModelId(const std::string& modelId) {
            return modelId.find(':') == std::string::npos ? "minecraft:" + modelId : modelId;
        }

        std::shared_ptr<const nlohmann::json> GetDefinition(const std::string& modelId) {
            const std::string id = NormalizeModelId(modelId);
            DefinitionCache& cache = Definitions();
            std::lock_guard<std::mutex> lock(cache.mutex);
            if (auto it = cache.byId.find(id); it != cache.byId.end()) return it->second;
            std::shared_ptr<const nlohmann::json> def;
            // The engine tree folds the minecraft namespace (resource packs'
            // assets/minecraft/items override it through GetAssetPath).
            // Other namespaces have no item definitions here.
            if (id.rfind("minecraft:", 0) == 0) {
                const std::string path = PlatformMain::GetAssetPath("assets/items/" + id.substr(10) + ".json");
                std::ifstream f(path);
                if (f.is_open()) {
                    try {
                        auto root = std::make_shared<nlohmann::json>();
                        f >> *root;
                        if (root->is_object() && root->contains("model")) def = std::move(root);
                    } catch (const std::exception& e) {
                        Log::Warning("[ClientItemLoader] %s parse error: %s", path.c_str(), e.what());
                    }
                }
            }
            cache.byId.emplace(id, def);
            return def;
        }

        std::string PropertyOf(const nlohmann::json& node) {
            auto p = node.find("property");
            return p != node.end() && p->is_string() ? StripNamespace(p->get<std::string>()) : std::string();
        }

        int IndexOf(const nlohmann::json& node) {
            auto ix = node.find("index");
            return ix != node.end() && ix->is_number_integer() ? static_cast<int>(ix->get<int64_t>()) : 0;
        }

        bool BoolField(const nlohmann::json& node, const char* key, bool fallback) {
            auto f = node.find(key);
            return f != node.end() && f->is_boolean() ? f->get<bool>() : fallback;
        }

        // A ConditionalItemModelProperty the stack answers; nullopt when the
        // property reads world / holder state (the flat summary's default).
        std::optional<bool> ConditionValue(const nlohmann::json& node, const ItemStack& stack) {
            const std::string prop = PropertyOf(node);
            if (prop == "custom_model_data") {
                const auto cmd = stack.get(DataComponents::CUSTOM_MODEL_DATA);
                return cmd && cmd->GetFlag(IndexOf(node)).value_or(false);
            }
            if (prop == "has_component") {
                auto c = node.find("component");
                if (c == node.end() || !c->is_string()) return false;
                std::string name = c->get<std::string>();
                if (name.rfind("minecraft:", 0) == 0) name.erase(0, 10);
                const DataComponentTypeBase* type = DataComponents::ByName(name);
                if (!type) return false;
                return BoolField(node, "ignore_default", false) ? stack.components.has(*type) : stack.has(*type);
            }
            if (prop == "damaged") return IsDamaged(stack);
            if (prop == "broken")  return NextDamageWillBreak(stack);
            return std::nullopt;
        }

        // A SelectItemModelProperty the stack answers: outer nullopt = not
        // stack state; inner nullopt = no value (the fallback).
        std::optional<std::optional<std::string>> SelectValue(const nlohmann::json& node, const ItemStack& stack) {
            if (PropertyOf(node) == "custom_model_data") {
                const auto cmd = stack.get(DataComponents::CUSTOM_MODEL_DATA);
                return cmd ? cmd->GetString(IndexOf(node)) : std::optional<std::string>{};
            }
            // TrimMaterialProperty: the stack's TRIM material key.
            if (PropertyOf(node) == "trim_material") {
                const auto trim = stack.get(DataComponents::TRIM);
                if (!trim) return std::optional<std::string>{};
                const std::string& m = trim->material;
                return std::optional<std::string>(m.find(':') == std::string::npos ? "minecraft:" + m : m);
            }
            return std::nullopt;
        }

        // A RangeSelectItemModelProperty the stack answers.
        std::optional<float> RangeValue(const nlohmann::json& node, const ItemStack& stack) {
            const std::string prop = PropertyOf(node);
            if (prop == "custom_model_data") {
                const auto cmd = stack.get(DataComponents::CUSTOM_MODEL_DATA);
                return cmd ? cmd->GetFloat(IndexOf(node)).value_or(0.0f) : 0.0f;
            }
            if (prop == "damage") {
                const float damage = static_cast<float>(GetDamageValue(stack));
                const float maxDamage = static_cast<float>(GetMaxDamage(stack));
                if (BoolField(node, "normalize", true)) {
                    return maxDamage > 0.0f ? std::clamp(damage / maxDamage, 0.0f, 1.0f) : 0.0f;
                }
                return std::clamp(damage, 0.0f, maxDamage);
            }
            if (prop == "count") {
                const float count = static_cast<float>(stack.count);
                const float maxCount = static_cast<float>(GetMaxStackSize(stack));   // Count: stack.getMaxStackSize()
                if (BoolField(node, "normalize", true)) {
                    return maxCount > 0.0f ? std::clamp(count / maxCount, 0.0f, 1.0f) : 0.0f;
                }
                return std::clamp(count, 0.0f, maxCount);
            }
            return std::nullopt;
        }

        bool WhenMatches(const nlohmann::json& when, const std::string& value) {
            if (when.is_string()) return when.get<std::string>() == value;
            if (when.is_array()) {
                for (const auto& w : when) if (w.is_string() && w.get<std::string>() == value) return true;
            }
            return false;
        }

        // MC ItemModel.update down the branches the stack decides; returns
        // the node where the stack stops deciding.
        const nlohmann::json* Descend(const nlohmann::json& node, const ItemStack& stack, int depth) {
            if (depth > 64 || !node.is_object()) return &node;
            auto typeIt = node.find("type");
            if (typeIt == node.end() || !typeIt->is_string()) return &node;
            const std::string type = StripNamespace(typeIt->get<std::string>());
            if (type == "condition") {
                const std::optional<bool> v = ConditionValue(node, stack);
                if (!v) return &node;
                auto branch = node.find(*v ? "on_true" : "on_false");
                return branch != node.end() ? Descend(*branch, stack, depth + 1) : &node;
            }
            if (type == "select") {
                const auto v = SelectValue(node, stack);
                if (!v) return &node;
                if (*v) {
                    auto cases = node.find("cases");
                    if (cases != node.end() && cases->is_array()) {
                        for (const auto& c : *cases) {
                            if (!c.is_object()) continue;
                            auto when = c.find("when");
                            auto model = c.find("model");
                            if (when != c.end() && model != c.end() && WhenMatches(*when, **v)) {
                                return Descend(*model, stack, depth + 1);
                            }
                        }
                    }
                }
                auto fb = node.find("fallback");
                return fb != node.end() ? Descend(*fb, stack, depth + 1) : &node;
            }
            if (type == "range_dispatch") {
                const std::optional<float> raw = RangeValue(node, stack);
                if (!raw) return &node;
                auto sc = node.find("scale");
                const float value = *raw * (sc != node.end() && sc->is_number() ? sc->get<float>() : 1.0f);
                // lastIndexLessOrEqual over the entries sorted by threshold.
                const nlohmann::json* chosen = nullptr;
                float best = -std::numeric_limits<float>::infinity();
                auto entries = node.find("entries");
                if (!std::isnan(value) && entries != node.end() && entries->is_array()) {
                    for (const auto& e : *entries) {
                        if (!e.is_object()) continue;
                        auto th = e.find("threshold");
                        auto model = e.find("model");
                        if (th == e.end() || !th->is_number() || model == e.end()) continue;
                        const float t = th->get<float>();
                        if (t <= value && (chosen == nullptr || t >= best)) { best = t; chosen = &*model; }
                    }
                }
                if (chosen) return Descend(*chosen, stack, depth + 1);
                auto fb = node.find("fallback");
                return fb != node.end() ? Descend(*fb, stack, depth + 1) : &node;
            }
            return &node;
        }

    } // namespace

    bool ClientItemLoader::DefinitionExists(const std::string& modelId) {
        return GetDefinition(modelId) != nullptr;
    }

    bool ClientItemLoader::ReadsCustomModelData(const std::string& modelId) {
        const auto def = GetDefinition(modelId);
        return def && ReadsCmd((*def)["model"], 0);
    }

    ClientItemDesc ClientItemLoader::Evaluate(const std::string& modelId, const ItemStack& stack,
                                              std::string& leafKey) {
        ClientItemDesc desc;
        leafKey.clear();
        const auto def = GetDefinition(modelId);
        if (!def) return desc;
        const nlohmann::json& root = def->at("model");
        const nlohmann::json* leaf = Descend(root, stack, 0);
        bool foundFrames = false;
        Resolve(*leaf, desc, foundFrames);
        desc.readsCustomModelData = ReadsCmd(root, 0);
        char address[32];
        std::snprintf(address, sizeof(address), "%p", static_cast<const void*>(leaf));
        leafKey = NormalizeModelId(modelId) + "@" + address;
        return desc;
    }

} // namespace Game
