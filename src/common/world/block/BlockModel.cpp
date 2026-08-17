// File: src/common/world/block/BlockModel.cpp
#include "BlockModel.hpp"
#include "../../core/Log.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
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

            // **OPTIMIZATION**: Early-out for no-op children that only redirect to parent.
            // `ambientocclusion` counts as content: a child may override nothing but
            // that flag, and returning the parent verbatim would silently drop it.
            if (!j.contains("textures") && !j.contains("elements")
                && !j.contains("ambientocclusion")) {
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

        // MC's `ambientocclusion` is a nullable Boolean: present means override,
        // absent means keep whatever the parent chain resolved to. `result` is
        // already the resolved parent at this point, so inheritance is implicit.
        if (j.contains("ambientocclusion") && j["ambientocclusion"].is_boolean()) {
            result.ambientOcclusion = j["ambientocclusion"].get<bool>();
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
                    faceDef.SetCullface(faceJson["cullface"].get<std::string>());
                }

                // Per-face texture rotation (optional). MC only accepts
                // 0/90/180/270 (Quadrant.CODEC rejects anything else), so
                // normalise and drop garbage rather than emitting a skewed
                // face. The mesher applies it as a UV-corner permutation.
                if (faceJson.contains("rotation")) {
                    const int raw = faceJson["rotation"].get<int>();
                    if (raw % 90 == 0) {
                        faceDef.uvRotation = ((raw % 360) + 360) % 360;
                    } else {
                        Log::Warning("Model face rotation %d is not a multiple of 90 - ignoring", raw);
                    }
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
            face.SetCullface(FaceDirToString(dir)); // Enable culling for all faces

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

        // Model not found — fall back to s_defaultModel (a stone cube).
        // Reached from mesh worker threads, so it stays allocation- and
        // lock-free: anything that writes shared state here serializes every
        // mesher on the same mutex.
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

    void BlockModelRegistry::RegisterModel(const std::string& name, BlockModel model) {
        s_models[name] = std::move(model);
    }

    // ========================================================================
    // MODEL ROTATION  (MC BlockModelRotation + FaceBakery, applied at load)
    // ========================================================================

    namespace {

        // Integer 3-vector rotations, quarter turns only. Deriving both the
        // geometry and the UV bookkeeping from one shared transform is what
        // keeps them consistent — hand-tabulating each case is where this kind
        // of code normally goes wrong.
        struct IVec3 { int x, y, z; };

        constexpr bool operator==(IVec3 a, IVec3 b) { return a.x==b.x && a.y==b.y && a.z==b.z; }

        // x:90 tips the model so UP→NORTH, NORTH→DOWN, DOWN→SOUTH, SOUTH→UP;
        // X is fixed. MC's blockstate `x` is a NEGATIVE quarter-turn about +X
        // (BlockModelRotation composes the orientation from Quadrant angles the
        // same way ModelRotation historically did with `-x`), so this is
        // (x, z, -y) and not (x, -z, y).
        //
        // This had the opposite sign, and the log example the comment used to
        // cite cannot catch it: `axis=z` only needs the end grain to land
        // somewhere on the Z axis, and both signs do that. The first block that
        // discriminates is an ASYMMETRIC x-rotated model — a wall button, which
        // came out on the opposite wall from the one it was placed against.
        //
        // Cross-checked against code rather than convention:
        // ButtonBlock.makeShapes gives `face=wall,facing=north` the box
        // z∈[8,16] (the cell's SOUTH half), and blockstates/oak_button.json
        // reaches that state with `x:90` applied to a floor-hugging model.
        // Only a -90 turn moves the model from y≈1 to z≈15 to match.
        constexpr IVec3 RotX90(IVec3 v) { return { v.x, v.z, -v.y }; }

        // y:90 turns NORTH→EAST→SOUTH→WEST; Y is fixed. A furnace's
        // `facing=east` variant is the plain (north-facing) model with
        // `"y": 90`, so this must carry the front face from north to east.
        constexpr IVec3 RotY90(IVec3 v) { return { -v.z, v.y, v.x }; }

        IVec3 ApplyRot(IVec3 v, int xTurns, int yTurns) {
            for (int i = 0; i < xTurns; ++i) v = RotX90(v);
            for (int i = 0; i < yTurns; ++i) v = RotY90(v);
            return v;
        }

        IVec3 NormalOf(FaceDir d) {
            switch (d) {
                case FaceDir::Up:    return { 0,  1,  0};
                case FaceDir::Down:  return { 0, -1,  0};
                case FaceDir::North: return { 0,  0, -1};
                case FaceDir::South: return { 0,  0,  1};
                case FaceDir::West:  return {-1,  0,  0};
                case FaceDir::East:  return { 1,  0,  0};
            }
            return {0, 1, 0};
        }

        FaceDir FaceFromNormal(IVec3 n) {
            if (n == IVec3{ 0,  1,  0}) return FaceDir::Up;
            if (n == IVec3{ 0, -1,  0}) return FaceDir::Down;
            if (n == IVec3{ 0,  0, -1}) return FaceDir::North;
            if (n == IVec3{ 0,  0,  1}) return FaceDir::South;
            if (n == IVec3{-1,  0,  0}) return FaceDir::West;
            return FaceDir::East;
        }

        // The world-space U and V axes each face's texture runs along, read
        // straight off Mesher::CreateFaceVertices' corner assignment. If that
        // winding ever changes, these must change with it or rotated models
        // will come out with mirrored or 90°-off textures.
        void FaceUvAxes(FaceDir d, IVec3& u, IVec3& v) {
            switch (d) {
                case FaceDir::Up:    u = { 1, 0, 0}; v = {0,  0,  1}; break;
                case FaceDir::Down:  u = { 1, 0, 0}; v = {0,  0, -1}; break;
                case FaceDir::South: u = { 1, 0, 0}; v = {0, -1,  0}; break;
                case FaceDir::North: u = {-1, 0, 0}; v = {0, -1,  0}; break;
                case FaceDir::East:  u = { 0, 0,-1}; v = {0, -1,  0}; break;
                case FaceDir::West:  u = { 0, 0, 1}; v = {0, -1,  0}; break;
            }
        }

        // One step of the mesher's uvRotation shift, expressed on the face's
        // (U,V) axis pair. Derived from the corner permutation in
        // CreateFaceVertices: shift=1 sends (U,V) to (V,-U).
        void UvShiftOnce(IVec3& u, IVec3& v) {
            const IVec3 oldU = u;
            u = v;
            v = { -oldU.x, -oldU.y, -oldU.z };
        }

        // How many uvRotation steps the destination face needs so its texture
        // ends up oriented the way the source face's texture was carried by the
        // rotation. Solved by search over the four possibilities rather than
        // tabulated — there are only four, and the search cannot silently
        // disagree with UvShiftOnce the way a hand-written table can.
        int SolveUvShift(FaceDir dstFace, IVec3 rotatedU, IVec3 rotatedV) {
            IVec3 u, v;
            FaceUvAxes(dstFace, u, v);
            for (int s = 0; s < 4; ++s) {
                if (u == rotatedU && v == rotatedV) return s * 90;
                UvShiftOnce(u, v);
            }
            return 0; // unreachable for proper quarter turns
        }

        // Rotate a point in MC model space (0..16) about the block centre.
        glm::vec3 RotPoint(const glm::vec3& p, int xTurns, int yTurns) {
            glm::vec3 c = p - glm::vec3(8.0f);
            // Must match RotX90's sign exactly — geometry and face normals are
            // rotated by separate code paths and disagreeing puts the textures
            // on the wrong faces.
            for (int i = 0; i < xTurns; ++i) c = glm::vec3( c.x,  c.z, -c.y);
            for (int i = 0; i < yTurns; ++i) c = glm::vec3(-c.z,  c.y,  c.x);
            return c + glm::vec3(8.0f);
        }

    } // namespace

    BlockModel BlockModelRegistry::RotateModel(const BlockModel& src, int xQuarterTurns,
                                               int yQuarterTurns) {
        const int xt = ((xQuarterTurns % 4) + 4) % 4;
        const int yt = ((yQuarterTurns % 4) + 4) % 4;
        if (xt == 0 && yt == 0) return src;

        BlockModel out = src;
        out.elements.clear();
        out.elements.reserve(src.elements.size());

        for (const Element& e : src.elements) {
            Element r = e;

            // Geometry: rotate both corners, then re-normalise. Quarter turns
            // keep an axis-aligned box axis-aligned, but they can swap which
            // corner is the minimum, and `from` must stay <= `to`.
            const glm::vec3 a = RotPoint(e.from, xt, yt);
            const glm::vec3 b = RotPoint(e.to,   xt, yt);
            r.from = glm::min(a, b);
            r.to   = glm::max(a, b);

            // Faces move to the direction their normal rotates onto, carrying
            // their texture — with a uvRotation correction, because the mesher
            // derives UVs from fixed world-space axes per face rather than from
            // the vertices (MC gets this for free: its UVs ride the vertices).
            r.faces.clear();
            for (const auto& [dir, face] : e.faces) {
                const IVec3 n  = ApplyRot(NormalOf(dir), xt, yt);
                const FaceDir dstDir = FaceFromNormal(n);

                IVec3 su, sv;
                FaceUvAxes(dir, su, sv);
                // Carry the source face's existing uvRotation through, then add
                // whatever the geometric rotation demands.
                for (int s = 0; s < (((face.uvRotation / 90) % 4 + 4) % 4); ++s) UvShiftOnce(su, sv);

                FaceDef f = face;
                f.uvRotation = SolveUvShift(dstDir, ApplyRot(su, xt, yt), ApplyRot(sv, xt, yt));

                // Cullface is a world direction and must rotate with the face,
                // or a rotated block stops culling against its neighbours (or
                // culls against the wrong one and shows holes).
                if (!f.cullface.empty()) {
                    const FaceDir cullDir = ParseFaceDir(f.cullface);
                    f.SetCullface(FaceDirToString(FaceFromNormal(ApplyRot(NormalOf(cullDir), xt, yt))));
                }

                r.faces[dstDir] = std::move(f);
            }

            // Per-element rotation (the ±22.5/45 model-space kind). Its origin
            // rotates with the geometry and its axis maps to whichever axis it
            // becomes. Rare among the blocks that carry blockstate rotations,
            // but dropping it silently would deform e.g. a rotated rail.
            if (r.rotation.axis != 0) {
                r.rotation.origin = RotPoint(e.rotation.origin, xt, yt);
                IVec3 axisVec{ e.rotation.axis == 'x' ? 1 : 0,
                               e.rotation.axis == 'y' ? 1 : 0,
                               e.rotation.axis == 'z' ? 1 : 0 };
                const IVec3 ra = ApplyRot(axisVec, xt, yt);
                if      (ra.x != 0) { r.rotation.axis = 'x'; if (ra.x < 0) r.rotation.angle = -r.rotation.angle; }
                else if (ra.y != 0) { r.rotation.axis = 'y'; if (ra.y < 0) r.rotation.angle = -r.rotation.angle; }
                else                { r.rotation.axis = 'z'; if (ra.z < 0) r.rotation.angle = -r.rotation.angle; }
            }

            out.elements.push_back(std::move(r));
        }

        return out;
    }

    glm::vec3 ApplyElementRotation(const glm::vec3& point, const ElementRotation& rot,
                                   float scale) {
        if (rot.IsIdentity()) return point;

        const float rad = glm::radians(rot.angle);
        const float c = std::cos(rad);
        const float s = std::sin(rad);

        // `rescale` grows the two axes perpendicular to the rotation so the
        // turned geometry still spans its original bounds — MC computes it as
        // 1/max|component| of each transformed axis unit vector
        // (BlockElementRotation.scaleFactorForAxis), which for a single-axis
        // turn is exactly 1/|cos| off-axis and 1 on-axis. Used by diagonal
        // rails and stairs; the flowerbed stems leave it off.
        const float k = (rot.rescale && std::abs(c) > 1e-6f) ? 1.0f / std::abs(c) : 1.0f;

        const glm::vec3 origin = rot.origin * scale;
        const glm::vec3 v = point - origin;

        glm::vec3 r;
        switch (rot.axis) {
            case 'x': r = { v.x,        (v.y * c - v.z * s) * k, (v.y * s + v.z * c) * k }; break;
            case 'y': r = { (v.x * c + v.z * s) * k, v.y,        (-v.x * s + v.z * c) * k }; break;
            case 'z': r = { (v.x * c - v.y * s) * k, (v.x * s + v.y * c) * k, v.z        }; break;
            default:  return point;
        }
        return origin + r;
    }

    BlockModel BlockModelRegistry::MergeModels(const std::vector<const BlockModel*>& parts) {
        BlockModel out;
        if (parts.empty()) return out;
        if (parts.size() == 1) return *parts[0];

        // Inherit presentation from the first part — multipart entries describe
        // pieces of ONE block, so their display transforms agree.
        out.guiDisplay = parts[0]->guiDisplay;
        // Same for AO: MC reads it off `parts.getFirst()` alone
        // (ModelBlockRenderer.java:42) rather than combining across parts.
        out.ambientOcclusion = parts[0]->ambientOcclusion;

        // Each part carries its own `textures` map, and the same key ("#texture",
        // "#flowerbed") routinely means a different sprite in different parts.
        // Merging the maps would therefore silently repaint quads. Instead every
        // face reference is resolved against ITS OWN part first, and the merged
        // model gets identity entries keyed by the resolved path — after which
        // ResolveTexture on the merged model is a no-op passthrough and no key
        // can collide.
        for (const BlockModel* part : parts) {
            if (!part) continue;

            for (const Element& e : part->elements) {
                Element merged = e;
                merged.faces.clear();

                for (const auto& [dir, face] : e.faces) {
                    FaceDef f = face;
                    const std::string resolved = part->ResolveTexture(face.textureRef);
                    out.textures[resolved] = resolved;      // identity entry
                    if (part->translucentTextureRefs.count(
                            face.textureRef.empty() || face.textureRef[0] != '#'
                                ? face.textureRef
                                : face.textureRef.substr(1))) {
                        out.translucentTextureRefs.insert(resolved);
                    }
                    f.textureRef = "#" + resolved;
                    merged.faces[dir] = std::move(f);
                }

                out.elements.push_back(std::move(merged));
            }
        }

        // Particle texture: first part that declares one wins, resolved the
        // same way so it survives the key rewrite above.
        for (const BlockModel* part : parts) {
            if (!part) continue;
            const std::string particle = part->ResolveTexture("#particle");
            if (particle != "missingno") {
                out.textures["particle"] = particle;
                break;
            }
        }

        return out;
    }

} // namespace Game