// File: src/game/BlockModel.cpp
#include "BlockModel.hpp"
#include "../core/Log.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Game {

    // Static member definitions
    std::unordered_map<std::string, BlockModel> BlockModelRegistry::s_models;
    BlockModel BlockModelRegistry::s_defaultModel;

    bool BlockModelRegistry::LoadModels(const std::string& modelsPath) {
        Log::Info("Loading block models from: %s", modelsPath.c_str());

        // Clear existing models
        s_models.clear();
        CreateDefaultModel();

        // Check if directory exists
        if (!std::filesystem::exists(modelsPath)) {
            Log::Warning("Block models directory does not exist: %s", modelsPath.c_str());
            Log::Info("Using default cube model for all blocks");
            return false;
        }

        int loadedCount = 0;
        int failedCount = 0;

        try {
            // Iterate through all JSON files in the directory
            for (const auto& entry : std::filesystem::directory_iterator(modelsPath)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                    continue;
                }

                std::string filename = entry.path().stem().string();
                std::string filepath = entry.path().string();

                Log::Debug("Loading model: %s", filename.c_str());

                try {
                    BlockModel model = LoadModelFromFile(filepath);
                    s_models[filename] = std::move(model);
                    loadedCount++;

                    Log::Debug("Successfully loaded model '%s' with %zu elements",
                              filename.c_str(), s_models[filename].elements.size());
                } catch (const std::exception& e) {
                    Log::Error("Failed to load model '%s': %s", filename.c_str(), e.what());
                    failedCount++;
                }
            }
        } catch (const std::exception& e) {
            Log::Error("Error iterating models directory: %s", e.what());
            return false;
        }

        Log::Info("Block model loading complete: %d loaded, %d failed", loadedCount, failedCount);

        // Log some statistics
        for (const auto& [name, model] : s_models) {
            bool usesTinting = model.UsesBiomeTinting();
            auto textures = model.GetAllTexturePaths();

            Log::Debug("Model '%s': %zu elements, %zu unique textures, biome tinting: %s",
                      name.c_str(), model.elements.size(), textures.size(),
                      usesTinting ? "yes" : "no");
        }

        return loadedCount > 0;
    }

    BlockModel BlockModelRegistry::LoadModelFromFile(const std::string& filePath) {
        // Read JSON file
        std::ifstream file(filePath);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open file: " + filePath);
        }

        json j;
        try {
            file >> j;
        } catch (const json::exception& e) {
            throw std::runtime_error("JSON parse error: " + std::string(e.what()));
        }

        BlockModel model;

        // Parse parent (for model inheritance - not fully implemented but recorded)
        if (j.contains("parent")) {
            model.parent = j["parent"].get<std::string>();
        }

        // Parse textures section
        if (j.contains("textures")) {
            for (const auto& [key, value] : j["textures"].items()) {
                model.textures[key] = value.get<std::string>();
                Log::Debug("  Texture mapping: %s -> %s", key.c_str(), value.get<std::string>().c_str());
            }
        }

        // Parse elements section
        if (j.contains("elements")) {
            for (const auto& elemJson : j["elements"]) {
                Element element;

                // Parse "from" and "to" coordinates (convert from 0-16 to 0-1 range)
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
                        }

                        // Parse texture reference
                        if (faceJson.contains("texture")) {
                            faceDef.textureRef = faceJson["texture"].get<std::string>();
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

                        Log::Debug("    Face %s: uv(%.1f,%.1f,%.1f,%.1f) texture=%s tint=%d cull=%s",
                                  FaceDirToString(dir).c_str(),
                                  faceDef.uv.x, faceDef.uv.y, faceDef.uv.z, faceDef.uv.w,
                                  faceDef.textureRef.c_str(), faceDef.tintIndex, faceDef.cullface.c_str());
                    }
                }

                model.elements.push_back(element);
                Log::Debug("  Element: from(%.1f,%.1f,%.1f) to(%.1f,%.1f,%.1f) faces=%zu",
                          element.from.x, element.from.y, element.from.z,
                          element.to.x, element.to.y, element.to.z,
                          element.faces.size());
            }
        }

        // Validate model
        if (model.elements.empty()) {
            Log::Warning("Model has no elements, creating default cube");
            CreateDefaultModel();
            return s_defaultModel;
        }

        return model;
    }

    void BlockModelRegistry::CreateDefaultModel() {
        s_defaultModel = BlockModel();

        // Create default texture mappings
        s_defaultModel.textures["all"] = "block/stone";

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
            face.textureRef = "#all";
            face.tintIndex = -1; // No tinting
            face.cullface = FaceDirToString(dir); // Enable culling for all faces

            defaultElement.faces[dir] = face;
        }

        s_defaultModel.elements.push_back(defaultElement);

        Log::Debug("Created default cube model");
    }

    const BlockModel& BlockModelRegistry::GetModel(const std::string& name) {
        auto it = s_models.find(name);
        if (it != s_models.end()) {
            return it->second;
        }

        // Model not found, return default
        static bool loggedMissing = false;
        if (!loggedMissing) {
            Log::Debug("Model '%s' not found, using default cube model", name.c_str());
            loggedMissing = true;
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
        CreateDefaultModel();
    }

} // namespace Game