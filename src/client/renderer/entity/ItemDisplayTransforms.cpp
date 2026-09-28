// File: src/client/renderer/entity/ItemDisplayTransforms.cpp
#include "client/renderer/entity/ItemDisplayTransforms.hpp"

#include "client/resource/ResourcePacks.hpp"
#include "common/core/Log.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render::ItemDisplay {

    namespace {

        constexpr int kContextCount = 8;

        constexpr const char* kContextKeys[kContextCount] = {
            "thirdperson_lefthand", "thirdperson_righthand", "head", "ground", "fixed", "gui",
            "firstperson_lefthand", "firstperson_righthand",
        };

        struct ModelEntry {
            bool exists = false;
            // The chain reaches builtin/generated (a flat item model:
            // ItemModelGenerator's extruded layers).
            bool generated = false;
            // textures.layer0..3, as sprite names (the path's last segment).
            std::array<std::string, 4> layers{};
            // Each context resolved up the parent chain (nullopt = none
            // anywhere, before the left-hand fallback).
            std::array<std::optional<Transform>, kContextCount> own{};
            std::array<Transform, kContextCount> resolved{};
        };

        std::mutex g_mutex;
        std::unordered_map<std::string, ModelEntry> g_cache;
        int g_packGeneration = -1;

        // "minecraft:item/bow" / "item/bow" → "item/bow". A bare name is an
        // item model (MC's default for unqualified references in item
        // definitions is the namespace, not the folder, but every caller
        // here passes the folder).
        std::string Canonical(std::string_view path) {
            std::string s(path);
            if (const auto colon = s.find(':'); colon != std::string::npos) s = s.substr(colon + 1);
            return s;
        }

        glm::vec3 ReadVec3(const nlohmann::json& j, const glm::vec3& fallback) {
            if (!j.is_array() || j.size() != 3) return fallback;
            glm::vec3 v = fallback;
            for (int i = 0; i < 3; ++i) {
                if (j[static_cast<size_t>(i)].is_number()) v[i] = j[static_cast<size_t>(i)].get<float>();
            }
            return v;
        }

        // MC ItemTransform.Deserializer: translation clamped to ±80
        // sixteenths, scale to ±4.
        Transform ParseTransform(const nlohmann::json& j) {
            Transform t;
            t.identity = false;
            t.rotation = ReadVec3(j.value("rotation", nlohmann::json()), glm::vec3(0.0f));
            t.translation = glm::clamp(ReadVec3(j.value("translation", nlohmann::json()), glm::vec3(0.0f)),
                                       glm::vec3(-80.0f), glm::vec3(80.0f));
            t.scale = glm::clamp(ReadVec3(j.value("scale", nlohmann::json()), glm::vec3(1.0f)),
                                 glm::vec3(-4.0f), glm::vec3(4.0f));
            return t;
        }

        const ModelEntry& Load(const std::string& path, int depth);

        const ModelEntry& Load(const std::string& path, int depth) {
            if (auto it = g_cache.find(path); it != g_cache.end()) return it->second;
            ModelEntry entry;
            std::optional<std::string> parent;
            const std::string file = PlatformMain::GetAssetPath("assets/models/" + path + ".json");
            if (std::filesystem::exists(file)) {
                try {
                    std::ifstream in(file);
                    nlohmann::json j;
                    in >> j;
                    entry.exists = true;
                    if (j.contains("parent") && j["parent"].is_string()) {
                        parent = Canonical(j["parent"].get<std::string>());
                    }
                    if (j.contains("textures") && j["textures"].is_object()) {
                        const auto& textures = j["textures"];
                        for (size_t l = 0; l < entry.layers.size(); ++l) {
                            const std::string key = "layer" + std::to_string(l);
                            if (!textures.contains(key) || !textures[key].is_string()) continue;
                            std::string ref = textures[key].get<std::string>();
                            if (ref.empty() || ref[0] == '#') continue;
                            if (const auto slash = ref.rfind('/'); slash != std::string::npos) ref = ref.substr(slash + 1);
                            else if (const auto colon = ref.find(':'); colon != std::string::npos) ref = ref.substr(colon + 1);
                            entry.layers[l] = std::move(ref);
                        }
                    }
                    if (j.contains("display") && j["display"].is_object()) {
                        const auto& display = j["display"];
                        for (int c = 0; c < kContextCount; ++c) {
                            if (display.contains(kContextKeys[c]) && display[kContextKeys[c]].is_object()) {
                                entry.own[static_cast<size_t>(c)] = ParseTransform(display[kContextKeys[c]]);
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    Log::Warning("[ItemDisplay] bad model %s: %s", path.c_str(), e.what());
                }
            }
            // BlockModel's merge: this model's entries over the parent's
            // (builtin/* parents carry nothing).
            if (parent && *parent == "builtin/generated") entry.generated = true;
            if (parent && depth < 16 && parent->rfind("builtin/", 0) != 0) {
                const ModelEntry& p = Load(*parent, depth + 1);
                entry.generated = entry.generated || p.generated;
                for (size_t l = 0; l < entry.layers.size(); ++l) {
                    if (entry.layers[l].empty()) entry.layers[l] = p.layers[l];
                }
                for (int c = 0; c < kContextCount; ++c) {
                    if (!entry.own[static_cast<size_t>(c)]) entry.own[static_cast<size_t>(c)] = p.own[static_cast<size_t>(c)];
                }
            }
            // ItemTransforms: the left hands fall back to the right.
            for (int c = 0; c < kContextCount; ++c) {
                if (entry.own[static_cast<size_t>(c)]) entry.resolved[static_cast<size_t>(c)] = *entry.own[static_cast<size_t>(c)];
            }
            const size_t left = static_cast<size_t>(Context::ThirdPersonLeftHand);
            const size_t right = static_cast<size_t>(Context::ThirdPersonRightHand);
            if (!entry.own[left] && entry.own[right]) entry.resolved[left] = *entry.own[right];
            const size_t fpLeft = static_cast<size_t>(Context::FirstPersonLeftHand);
            const size_t fpRight = static_cast<size_t>(Context::FirstPersonRightHand);
            if (!entry.own[fpLeft] && entry.own[fpRight]) entry.resolved[fpLeft] = *entry.own[fpRight];
            return g_cache.emplace(path, std::move(entry)).first->second;
        }

        void DropStale() {
            if (Resources::CacheStale(g_packGeneration)) g_cache.clear();
        }

    } // namespace

    const Transform& Get(std::string_view modelPath, Context context) {
        std::lock_guard<std::mutex> lock(g_mutex);
        DropStale();
        const ModelEntry& e = Load(Canonical(modelPath), 0);
        return e.resolved[static_cast<size_t>(context)];
    }

    bool ModelExists(std::string_view modelPath) {
        std::lock_guard<std::mutex> lock(g_mutex);
        DropStale();
        return Load(Canonical(modelPath), 0).exists;
    }

    std::string SpriteLayer(std::string_view modelPath, int layer) {
        if (layer < 0 || layer >= 4) return {};
        std::lock_guard<std::mutex> lock(g_mutex);
        DropStale();
        return Load(Canonical(modelPath), 0).layers[static_cast<size_t>(layer)];
    }

    bool IsGenerated(std::string_view modelPath) {
        std::lock_guard<std::mutex> lock(g_mutex);
        DropStale();
        return Load(Canonical(modelPath), 0).generated;
    }

    glm::mat4 Apply(const glm::mat4& pose, const Transform& t, bool leftHand) {
        // 26.3 ItemTransform.apply ends with the model's centring
        // translate(-0.5, -0.5, -0.5) — NO_TRANSFORM included.
        if (t.identity) return glm::translate(pose, glm::vec3(-0.5f));
        glm::vec3 rot = t.rotation;
        float flip = 1.0f;
        if (leftHand) {
            rot.y = -rot.y;
            rot.z = -rot.z;
            flip = -1.0f;
        }
        glm::mat4 m = glm::translate(pose, glm::vec3(flip * t.translation.x, t.translation.y, t.translation.z) * 0.0625f);
        // Quaternionf.rotationXYZ: X, then Y, then Z (intrinsic).
        m = glm::rotate(m, glm::radians(rot.x), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(rot.y), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, glm::radians(rot.z), glm::vec3(0.0f, 0.0f, 1.0f));
        m = glm::scale(m, t.scale);
        return glm::translate(m, glm::vec3(-0.5f));
    }

} // namespace Render::ItemDisplay
