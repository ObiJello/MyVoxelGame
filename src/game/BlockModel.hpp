// File: src/game/BlockModel.hpp (FIXED - Parent Model Support)
#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <nlohmann/json.hpp>

namespace Game {

    // Which face of a cuboid we're talking about
    enum class FaceDir {
        Up = 0,    // +Y (top)
        Down = 1,  // -Y (bottom)
        North = 2, // -Z (front)
        South = 3, // +Z (back)
        West = 4,  // -X (left)
        East = 5   // +X (right)
    };

    // Convert string to FaceDir enum
    inline FaceDir ParseFaceDir(const std::string& face) {
        if (face == "up") return FaceDir::Up;
        if (face == "down") return FaceDir::Down;
        if (face == "north") return FaceDir::North;
        if (face == "south") return FaceDir::South;
        if (face == "west") return FaceDir::West;
        if (face == "east") return FaceDir::East;

        // Default fallback
        return FaceDir::Up;
    }

    // Convert FaceDir to string for debugging
    inline std::string FaceDirToString(FaceDir dir) {
        switch (dir) {
            case FaceDir::Up: return "up";
            case FaceDir::Down: return "down";
            case FaceDir::North: return "north";
            case FaceDir::South: return "south";
            case FaceDir::West: return "west";
            case FaceDir::East: return "east";
            default: return "unknown";
        }
    }

    // Per-face data as read from JSON
    struct FaceDef {
        glm::vec4 uv{0.0f, 0.0f, 16.0f, 16.0f}; // [u1, v1, u2, v2] in pixels (default full face)
        std::string textureRef;                   // e.g. "#side" → lookup in BlockModel::textures
        int tintIndex = -1;                       // -1 = no tint, 0+ = biome tinting
        std::string cullface;                     // e.g. "north", empty if no culling

        FaceDef() = default;
        FaceDef(const glm::vec4& uvCoords, const std::string& texture, int tint = -1, const std::string& cull = "")
            : uv(uvCoords), textureRef(texture), tintIndex(tint), cullface(cull) {}
    };

    // One cuboid "element" of the model (Minecraft models can have multiple cuboids)
    struct Element {
        glm::vec3 from{0.0f};                           // Bottom-left-back corner in 0-16 model space
        glm::vec3 to{16.0f};                            // Top-right-front corner in 0-16 model space
        std::map<FaceDir, FaceDef> faces;               // Only faces that are actually defined

        Element() = default;
        Element(const glm::vec3& fromPos, const glm::vec3& toPos) : from(fromPos), to(toPos) {}

        // Check if a specific face is defined
        bool HasFace(FaceDir dir) const {
            return faces.find(dir) != faces.end();
        }

        // Get face definition (assumes face exists)
        const FaceDef& GetFace(FaceDir dir) const {
            return faces.at(dir);
        }
    };

    // Full model for one block type
    struct BlockModel {
        std::string parent;                                    // Parent model (for inheritance)
        std::map<std::string, std::string> textures;          // Texture variable definitions
        std::vector<Element> elements;                         // List of cuboid elements

        BlockModel() = default;

        // Resolve a texture reference like "#side" to actual texture path
        std::string ResolveTexture(const std::string& textureRef) const {
            if (textureRef.empty() || textureRef[0] != '#') {
                return textureRef; // Not a reference, return as-is
            }

            std::string key = textureRef.substr(1); // Remove '#'
            auto it = textures.find(key);
            if (it != textures.end()) {
                return it->second;
            }

            // Fallback for missing texture references
            return "missingno";
        }

        // Get all unique texture paths used by this model
        std::vector<std::string> GetAllTexturePaths() const {
            std::vector<std::string> paths;

            for (const auto& element : elements) {
                for (const auto& [dir, face] : element.faces) {
                    std::string path = ResolveTexture(face.textureRef);
                    // Add to list if not already present
                    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
                        paths.push_back(path);
                    }
                }
            }

            return paths;
        }

        // Check if this model uses biome tinting
        bool UsesBiomeTinting() const {
            for (const auto& element : elements) {
                for (const auto& [dir, face] : element.faces) {
                    if (face.tintIndex >= 0) {
                        return true;
                    }
                }
            }
            return false;
        }
    };

    // Model registry - maps block names to their models
    class BlockModelRegistry {
    public:
        // Load all block models from the specified directory
        static bool LoadModels(const std::string& modelsPath = "assets/models/block");

        // Get a model by name (returns default if not found)
        static const BlockModel& GetModel(const std::string& name);

        // Check if a model exists
        static bool HasModel(const std::string& name);

        // Get list of all loaded model names
        static std::vector<std::string> GetLoadedModelNames();

        // Get number of loaded models
        static size_t GetModelCount();

        // Clear all loaded models
        static void Clear();

    private:
        static std::unordered_map<std::string, BlockModel> s_models;
        static std::unordered_map<std::string, BlockModel> s_parentModels; // NEW: Cache for parent models
        static std::unordered_map<std::string, nlohmann::json> s_rawJsons; // Raw JSON storage
        static BlockModel s_defaultModel;

        // Helper functions
        static BlockModel LoadModelFromFile(const std::string& filePath);
        static void CreateDefaultModel();

        // NEW: Parent model resolution functions
        static void LoadParentModels(const std::string& modelsPath);
        static BlockModel ResolveModel(const BlockModel& model);
        static BlockModel ResolveModelRecursive(const BlockModel& model, int depth);
    };

} // namespace Game