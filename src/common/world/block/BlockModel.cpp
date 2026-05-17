// File: src/common/world/block/BlockModel.cpp
#include "BlockModel.hpp"
#include "../../core/Log.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#ifdef __APPLE__
#include <unistd.h>
#endif
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Game {

    // Static member definitions
    std::unordered_map<std::string, BlockModel> BlockModelRegistry::s_models;
    std::unordered_map<std::string, nlohmann::json> BlockModelRegistry::s_rawJsons; // Raw JSON storage
    BlockModel BlockModelRegistry::s_defaultModel;

    bool BlockModelRegistry::LoadModels(const std::string& modelsPath) {
        Log::Info("Loading block models from: %s", modelsPath.c_str());

        #ifdef __APPLE__
        // DEBUG: Print current working directory
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            Log::Info("Current working directory: %s", cwd);
        }
        #endif

        // Clear existing models and raw JSON
        s_models.clear();
        s_rawJsons.clear();
        CreateDefaultModel();

        // Check if directory exists
        if (!std::filesystem::exists(modelsPath)) {
            Log::Warning("Block models directory does not exist: %s", modelsPath.c_str());
            Log::Info("Using default cube model for all blocks");
            return false;
        }

        int loadedJsonCount = 0;
        int resolvedModelCount = 0;
        int failedCount = 0;

        try {
            // PHASE 1: Load all raw JSON files into memory
            Log::Debug("Phase 1: Loading raw JSON files...");
            for (const auto& entry : std::filesystem::directory_iterator(modelsPath)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                    continue;
                }

                std::string filename = entry.path().stem().string();
                std::string filepath = entry.path().string();

                try {
                    std::ifstream file(filepath);
                    if (!file.is_open()) {
                        Log::Warning("Cannot open JSON file: %s", filepath.c_str());
                        continue;
                    }

                    json j;
                    file >> j;

                    // Store under base name only (canonicalization handles prefixed references)
                    s_rawJsons[filename] = j;                           // "grass_block"

                    loadedJsonCount++;
                    //Log::Debug("Loaded raw JSON: %s", filename.c_str());

                } catch (const std::exception& e) {
                    Log::Error("Failed to load JSON '%s': %s", filename.c_str(), e.what());
                    failedCount++;
                }
            }

            // PHASE 2: Resolve all models by following parent chains
            Log::Debug("Phase 2: Resolving model inheritance...");

            // Get all unique model names (just the base names, not the prefixed versions)
            std::set<std::string> modelNames;
            for (const auto& [key, json] : s_rawJsons) {
                // Only process base names (no slashes or prefixes)
                if (key.find('/') == std::string::npos && key.find(':') == std::string::npos) {
                    modelNames.insert(key);
                }
            }

            for (const std::string& modelName : modelNames) {
                try {
                    ResolveModel(modelName); // Model is cached inside ResolveModelRecursive
                    resolvedModelCount++;

                    //Log::Debug("Successfully resolved model '%s'", modelName.c_str());
                } catch (const std::exception& e) {
                    Log::Error("Failed to resolve model '%s': %s", modelName.c_str(), e.what());
                    failedCount++;
                }
            }

        } catch (const std::exception& e) {
            Log::Error("Error iterating models directory: %s", e.what());
            return false;
        }

        Log::Info("Block model loading complete: %d JSON files loaded, %d models resolved, %d failed",
                 loadedJsonCount, resolvedModelCount, failedCount);

        // Log some statistics
        for (const auto& [name, model] : s_models) {
            bool usesTinting = model.UsesBiomeTinting();
            auto textures = model.GetAllTexturePaths();

            /*Log::Debug("Model '%s': %zu elements, %zu unique textures, biome tinting: %s",
                      name.c_str(), model.elements.size(), textures.size(),
                      usesTinting ? "yes" : "no");*/
        }

        return resolvedModelCount > 0;
    }

    BlockModel BlockModelRegistry::ResolveModel(const std::string& name) {
        return ResolveModelRecursive(name, 0);
    }

    BlockModel BlockModelRegistry::ResolveModelRecursive(const std::string& name, int depth) {
        // Prevent infinite recursion
        if (depth > 16) {
            Log::Warning("Maximum parent resolution depth reached for model: %s", name.c_str());
            return s_defaultModel;
        }

        // Check if already cached
        auto itCached = s_models.find(name);
        if (itCached != s_models.end()) {
            return itCached->second;
        }

        // Find raw JSON
        auto itRaw = s_rawJsons.find(name);
        if (itRaw == s_rawJsons.end()) {
            Log::Debug("Model JSON not found: %s", name.c_str());
            return s_defaultModel;
        }

        const json& j = itRaw->second;
        //Log::Debug("Resolving model: %s (depth: %d)", name.c_str(), depth);

        // Start with parent if any
        BlockModel result;
        if (j.contains("parent") && !j["parent"].get<std::string>().empty()) {
            std::string parentRef = j["parent"].get<std::string>();

            // Canonicalize parent reference - strip to just the model name
            std::string parentName = CanonicalizeModelName(parentRef);

            //Log::Debug("  Parent: %s -> %s", parentRef.c_str(), parentName.c_str());
            result = ResolveModelRecursive(parentName, depth + 1);

            // **OPTIMIZATION**: Early-out for no-op children that only redirect to parent
            if (!j.contains("textures") && !j.contains("elements")) {
                Log::Debug("  No-op child model, returning parent directly");
                s_models[name] = result;
                return result;
            }
        } else {
            // No parent. If this JSON declares its own `elements`, start fresh — the
            // default cube geometry would only get overwritten anyway. Models that have
            // NO parent AND NO elements (e.g. MC's `chest.json`, `sign.json`, etc., which
            // are rendered by BlockEntityWithoutLevelRenderer instead of via the model
            // system) MUST NOT inherit the stone-cube geometry from s_defaultModel — that
            // would silently turn every BEWLR block into a stone cube in the inventory.
            // Leave such models as empty so the renderer can fall back to the particle
            // texture (matching MC's "missing custom renderer" behaviour).
            if (j.contains("elements")) {
                result = s_defaultModel;  // textures from default; elements overridden below
            }
            // else: result stays as a fresh empty BlockModel (no geometry, no textures)
        }

        // Merge this JSON's textures (child overrides parent).
        //
        // Two valid forms in MC's modern model schema:
        //   1. Bare string:   "all": "minecraft:block/stone"
        //   2. Object form:   "all": { "force_translucent": true,
        //                              "sprite": "minecraft:block/glass" }
        // The object form (used by glass.json, ice.json, etc.) lets MC mark a
        // texture as needing the translucent render path independent of the
        // block's `gui_light` field. The actual texture reference lives in
        // the inner `sprite` key. We extract that here; `force_translucent`
        // itself is captured into BlockModel.translucentTextureRefs so the
        // mesher can route those faces through the translucent pass.
        if (j.contains("textures")) {
            for (const auto& [key, value] : j["textures"].items()) {
                std::string texPath;
                if (value.is_string()) {
                    texPath = value.get<std::string>();
                } else if (value.is_object()) {
                    // Modern MC object form. Required field: "sprite".
                    auto spriteIt = value.find("sprite");
                    if (spriteIt == value.end() || !spriteIt->is_string()) {
                        // Malformed entry — skip silently rather than throw
                        // (one bad texture shouldn't kill the whole model).
                        continue;
                    }
                    texPath = spriteIt->get<std::string>();
                    // Optional: force_translucent flag. MC uses this on glass
                    // and similar to bypass the normal opacity classification
                    // and route the face through the translucent render layer
                    // even when the texture's alpha would otherwise look
                    // opaque to the atlas builder. We surface it by tagging
                    // the texture KEY so the mesher / model classifier can
                    // consult it later.
                    auto ftIt = value.find("force_translucent");
                    if (ftIt != value.end() && ftIt->is_boolean() && ftIt->get<bool>()) {
                        result.translucentTextureRefs.insert(key);
                    }
                } else {
                    // Number / array / null → skip (no valid form).
                    continue;
                }

                // **CANONICALIZATION**: Strip "minecraft:" namespace prefix to match atlas keys
                // "minecraft:block/stone" -> "block/stone"
                if (texPath.rfind("minecraft:", 0) == 0) {
                    texPath = texPath.substr(10); // Remove "minecraft:" prefix
                }

                result.textures[key] = texPath;
                //Log::Debug("  Texture: %s -> %s", key.c_str(), texPath.c_str());
            }
        }

        // Override elements if this JSON has any (completely replace parent elements)
        if (j.contains("elements")) {
            result.elements.clear();
            //Log::Debug("  Parsing %zu elements", j["elements"].size());

            for (const auto& elemJson : j["elements"]) {
                Element element = ParseElement(elemJson);
                result.elements.push_back(element);
            }
        }

        // Merge display.gui from this JSON onto whatever the parent provided. MC's
        // convention: any sub-key set on the child overrides ONLY that sub-key
        // (rotation, translation, scale are independently inheritable). The parent
        // chain bubbles up `block/block`'s defaults (rotation [30,225,0], scale 0.625)
        // for blocks that don't override; fence/gate/etc. set their own here.
        if (j.contains("display") && j["display"].is_object()
            && j["display"].contains("gui") && j["display"]["gui"].is_object()) {
            const auto& g = j["display"]["gui"];
            auto readVec3 = [](const json& arr, glm::vec3 fallback) {
                if (!arr.is_array() || arr.size() < 3) return fallback;
                return glm::vec3(arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>());
            };
            if (g.contains("rotation"))    result.guiDisplay.rotation    = readVec3(g["rotation"],    result.guiDisplay.rotation);
            if (g.contains("translation")) result.guiDisplay.translation = readVec3(g["translation"], result.guiDisplay.translation);
            if (g.contains("scale"))       result.guiDisplay.scale       = readVec3(g["scale"],       result.guiDisplay.scale);
        }

        // Clear parent reference to avoid confusion
        result.parent = "";

        // **FIX**: Expand template_single_face child models into full cubes.
        // MC uses blockstate multipart to rotate a single face to all 6 directions.
        // Since this game has no blockstate system, we expand child models that
        // inherit from template_single_face into full cubes at resolution time.
        // Only apply to children (not template_single_face itself) by checking
        // the parent reference in the JSON.
        if (j.contains("parent")) {
            std::string parentCanon = CanonicalizeModelName(j["parent"].get<std::string>());
            if (parentCanon == "template_single_face" && result.elements.size() == 1) {
                const auto& elem = result.elements[0];
                if (elem.faces.size() == 1 && elem.faces.count(FaceDir::North)) {

                    std::string texRef = elem.faces.at(FaceDir::North).textureRef;

                    // For mushroom stems, MC shows mushroom_block_inside on top/bottom
                    std::string resolvedTex = result.ResolveTexture(texRef);
                    bool isStem = (resolvedTex.find("mushroom_stem") != std::string::npos);

                    std::string topBottomRef = texRef;
                    if (isStem) {
                        result.textures["_inside"] = "block/mushroom_block_inside";
                        topBottomRef = "#_inside";
                    }

                    Element fullCube;
                    fullCube.from = glm::vec3(0, 0, 0);
                    fullCube.to = glm::vec3(16, 16, 16);
                    glm::vec4 defaultUV(0, 0, 16, 16);

                    fullCube.faces[FaceDir::North] = FaceDef(defaultUV, texRef, -1, "north");
                    fullCube.faces[FaceDir::South] = FaceDef(defaultUV, texRef, -1, "south");
                    fullCube.faces[FaceDir::East]  = FaceDef(defaultUV, texRef, -1, "east");
                    fullCube.faces[FaceDir::West]  = FaceDef(defaultUV, texRef, -1, "west");
                    fullCube.faces[FaceDir::Up]    = FaceDef(defaultUV, topBottomRef, -1, "up");
                    fullCube.faces[FaceDir::Down]  = FaceDef(defaultUV, topBottomRef, -1, "down");

                    result.elements.clear();
                    result.elements.push_back(fullCube);

                    Log::Debug("Expanded template_single_face child '%s' to full cube%s",
                              name.c_str(), isStem ? " (stem: inside top/bottom)" : "");
                }
            }
        }

        //Log::Debug("Resolved model '%s': %zu elements, %zu textures", name.c_str(), result.elements.size(), result.textures.size());

        // **OPTIMIZATION**: Cache the resolved model immediately to avoid redundant work
        // This matches Minecraft's exact behavior where each model is resolved only once
        s_models[name] = result;

        return result;
    }

    std::string BlockModelRegistry::CanonicalizeModelName(const std::string& modelRef) {
        // Strip prefixes to get just the model name
        // "minecraft:block/cube_all" -> "cube_all"
        // "block/cube" -> "cube"
        // "cube_all" -> "cube_all"

        auto slash = modelRef.find_last_of('/');
        if (slash != std::string::npos) {
            return modelRef.substr(slash + 1);
        }

        auto colon = modelRef.find_last_of(':');
        if (colon != std::string::npos) {
            return modelRef.substr(colon + 1);
        }

        return modelRef;
    }

    Element BlockModelRegistry::ParseElement(const nlohmann::json& elemJson) {
        Element element;

        // Parse "from" and "to" coordinates
        if (elemJson.contains("from") && elemJson["from"].is_array() && elemJson["from"].size() == 3) {
            element.from = glm::vec3(
                elemJson["from"][0].get<float>(),
                elemJson["from"][1].get<float>(),
                elemJson["from"][2].get<float>()
            );
        }

        if (elemJson.contains("to") && elemJson["to"].is_array() && elemJson["to"].size() == 3) {
            element.to = glm::vec3(
                elemJson["to"][0].get<float>(),
                elemJson["to"][1].get<float>(),
                elemJson["to"][2].get<float>()
            );
        }

        // Parse per-element rotation (MC model JSON `rotation` block). Used by
        // chains, rails, and any block whose geometry isn't axis-aligned.
        if (elemJson.contains("rotation") && elemJson["rotation"].is_object()) {
            const auto& rj = elemJson["rotation"];
            if (rj.contains("origin") && rj["origin"].is_array() && rj["origin"].size() == 3) {
                element.rotation.origin = glm::vec3(
                    rj["origin"][0].get<float>(),
                    rj["origin"][1].get<float>(),
                    rj["origin"][2].get<float>()
                );
            }
            if (rj.contains("axis") && rj["axis"].is_string()) {
                std::string axis = rj["axis"].get<std::string>();
                if (!axis.empty()) element.rotation.axis = axis[0];
            }
            if (rj.contains("angle")) {
                element.rotation.angle = rj["angle"].get<float>();
            }
            if (rj.contains("rescale") && rj["rescale"].is_boolean()) {
                element.rotation.rescale = rj["rescale"].get<bool>();
            }
        }

        // Parse `shade` (defaults to true). When false, the renderer skips MC's
        // per-direction face shading (used by chain/leaves/etc. where the model's
        // texture already bakes in highlight/shadow).
        if (elemJson.contains("shade") && elemJson["shade"].is_boolean()) {
            element.shade = elemJson["shade"].get<bool>();
        }

        // Parse faces
        if (elemJson.contains("faces")) {
            for (const auto& [faceName, faceJson] : elemJson["faces"].items()) {
                FaceDir dir = ParseFaceDir(faceName);
                FaceDef faceDef;

                // Parse UV coordinates
                if (faceJson.contains("uv") && faceJson["uv"].is_array() && faceJson["uv"].size() == 4) {
                    faceDef.uv = glm::vec4(
                        faceJson["uv"][0].get<float>(),
                        faceJson["uv"][1].get<float>(),
                        faceJson["uv"][2].get<float>(),
                        faceJson["uv"][3].get<float>()
                    );
                } else {
                    // Default to full face UV
                    faceDef.uv = glm::vec4(0.0f, 0.0f, 16.0f, 16.0f);
                }

                // CRITICAL FIX: Parse texture reference and strip leading '#' for clean storage
                if (faceJson.contains("texture")) {
                    std::string rawRef = faceJson["texture"].get<std::string>();
                    // Store without the '#' prefix for consistent lookup
                    faceDef.textureRef = (rawRef[0] == '#') ? rawRef.substr(1) : rawRef;
                }

                // Parse tint index (optional)
                if (faceJson.contains("tintindex")) {
                    faceDef.tintIndex = faceJson["tintindex"].get<int>();
                }

                // Parse cullface (optional)
                if (faceJson.contains("cullface")) {
                    faceDef.cullface = faceJson["cullface"].get<std::string>();
                }

                element.faces[dir] = faceDef;

                /*Log::Debug("    Face %s: uv(%.1f,%.1f,%.1f,%.1f) texture=%s tint=%d cull=%s",
                          FaceDirToString(dir).c_str(),
                          faceDef.uv.x, faceDef.uv.y, faceDef.uv.z, faceDef.uv.w,
                          faceDef.textureRef.c_str(), faceDef.tintIndex, faceDef.cullface.c_str());*/
            }
        }

        /*Log::Debug("  Element: from(%.1f,%.1f,%.1f) to(%.1f,%.1f,%.1f) faces=%zu",
                  element.from.x, element.from.y, element.from.z,
                  element.to.x, element.to.y, element.to.z,
                  element.faces.size());*/

        return element;
    }

    void BlockModelRegistry::CreateDefaultModel() {
        s_defaultModel = BlockModel();

        // Create comprehensive default texture mappings
        s_defaultModel.textures["all"] = "block/stone";
        s_defaultModel.textures["up"] = "block/stone";
        s_defaultModel.textures["down"] = "block/stone";
        s_defaultModel.textures["north"] = "block/stone";
        s_defaultModel.textures["south"] = "block/stone";
        s_defaultModel.textures["west"] = "block/stone";
        s_defaultModel.textures["east"] = "block/stone";
        s_defaultModel.textures["side"] = "block/stone";
        s_defaultModel.textures["top"] = "block/stone";
        s_defaultModel.textures["bottom"] = "block/stone";

        // Create a single full cube element
        Element defaultElement;
        defaultElement.from = glm::vec3(0.0f, 0.0f, 0.0f);
        defaultElement.to = glm::vec3(16.0f, 16.0f, 16.0f);

        // Add all six faces with default UVs
        std::vector<FaceDir> allFaces = {
            FaceDir::Up, FaceDir::Down, FaceDir::North,
            FaceDir::South, FaceDir::West, FaceDir::East
        };

        for (FaceDir dir : allFaces) {
            FaceDef face;
            face.uv = glm::vec4(0.0f, 0.0f, 16.0f, 16.0f); // Full face UV
            face.textureRef = "all"; // Clean reference (no '#' prefix)
            face.tintIndex = -1; // No tinting
            face.cullface = FaceDirToString(dir); // Enable culling for all faces

            defaultElement.faces[dir] = face;
        }

        s_defaultModel.elements.push_back(defaultElement);

        Log::Debug("Created default cube model with comprehensive texture mappings");
    }

    const BlockModel& BlockModelRegistry::GetModel(const std::string& name) {

        auto it = s_models.find(name);
        if (it != s_models.end()) {
            return it->second;
        }

        // ── Model not found — falling back to s_defaultModel (a stone cube). ──
        // Track every UNIQUE missing model name and append it to a text file
        // the user can audit. Without this, blocks with no model silently
        // render as stone cubes in-world and we have no idea which blocks
        // need attention.
        //
        // File: ./missing_block_models.txt (relative to the cwd the game was
        // launched from). One name per line; the file is APPENDED to as new
        // names are discovered, so it grows naturally as the user explores
        // chunks containing blocks we haven't modelled yet. Existing lines
        // are preserved across runs so you can collect a master list over
        // multiple sessions.
        //
        // Thread-safety: this gets hit from chunk-mesher worker threads, so
        // both the seen-set and the file write are mutex-guarded. The set
        // dedupes BEFORE touching the file, so the per-call cost after the
        // first occurrence is just a hash lookup + mutex.
        {
            static std::unordered_set<std::string> s_missingSeen;
            static std::mutex                      s_missingMutex;

            std::lock_guard<std::mutex> lock(s_missingMutex);
            if (s_missingSeen.insert(name).second) {
                // First time we've seen this name — write it out.
                const std::filesystem::path outPath = "missing_block_models.txt";
                std::error_code              ec;
                const std::filesystem::path  absPath = std::filesystem::absolute(outPath, ec);

                std::ofstream out(outPath, std::ios::app);
                if (out) {
                    out << name << '\n';
                    // Tell the user where the file lives the very first time
                    // we write to it, so they can find the audit list.
                    static bool s_announced = false;
                    if (!s_announced) {
                        s_announced = true;
                        Log::Info("[BlockModel] Tracking missing models in %s "
                                  "(first miss: '%s'). Each unique block model "
                                  "name that falls back to the stone cube is "
                                  "appended to that file.",
                                  absPath.string().c_str(), name.c_str());
                    }
                } else {
                    // File-open failure shouldn't be fatal — log once and move on.
                    static bool s_warnedOpenFail = false;
                    if (!s_warnedOpenFail) {
                        s_warnedOpenFail = true;
                        Log::Warning("[BlockModel] Could not open %s for append; "
                                     "missing-model audit list will be lost.",
                                     absPath.string().c_str());
                    }
                }
            }
        }

        return s_defaultModel;
    }

    bool BlockModelRegistry::HasModel(const std::string& name) {
        return s_models.find(name) != s_models.end();
    }

    std::vector<std::string> BlockModelRegistry::GetLoadedModelNames() {
        std::vector<std::string> names;
        names.reserve(s_models.size());

        for (const auto& [name, model] : s_models) {
            names.push_back(name);
        }

        return names;
    }

    size_t BlockModelRegistry::GetModelCount() {
        return s_models.size();
    }

    void BlockModelRegistry::Clear() {
        s_models.clear();
        s_rawJsons.clear();
        CreateDefaultModel();
    }

} // namespace Game