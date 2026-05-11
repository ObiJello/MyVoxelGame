// File: src/common/entity/ClientItemLoader.cpp
#include "ClientItemLoader.hpp"
#include "../core/Log.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

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
                    for (const auto& t : *tints) {
                        if (!t.is_object()) { out.layerTints.push_back(0); continue; }
                        auto def = t.find("default");
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
        return desc;
    }

} // namespace Game
