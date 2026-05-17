// File: src/common/world/block/BlockModel.hpp
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

    // Per-element rotation (MC model JSON `rotation` field). Only one axis at a time.
    // `axis == 0` means "no rotation" (the rotation is identity and can be skipped).
    // `origin` is in MC pixel space (0..16). MC restricts angle to ±22.5 / ±45 / 0;
    // we store and apply the raw value so non-vanilla models still work.
    // `rescale=true` (rare) scales the rotated geometry up so the rotated bounding
    // box fills the original axis-aligned bounds — used for diagonal stairs/rails;
    // not needed for chain/wall/etc., but parsed so we don't drop the field.
    struct ElementRotation {
        glm::vec3 origin{8.0f, 8.0f, 8.0f};
        char      axis    = 0;       // 'x' | 'y' | 'z' | 0
        float     angle   = 0.0f;    // degrees
        bool      rescale = false;
    };

    // One cuboid "element" of the model (Minecraft models can have multiple cuboids)
    struct Element {
        glm::vec3 from{0.0f};                           // Bottom-left-back corner in 0-16 model space
        glm::vec3 to{16.0f};                            // Top-right-front corner in 0-16 model space
        std::map<FaceDir, FaceDef> faces;               // Only faces that are actually defined
        ElementRotation rotation;                       // Optional per-element rotation
        bool       shade = true;                        // MC's `shade` field; false = no directional shading

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

    // MC's `display.gui` block — controls how the model is oriented + sized in the
    // inventory icon. Most blocks inherit `block/block`'s defaults (rotation [30, 225, 0],
    // scale [0.625]) but fences override to [30, 135, 0] (`fence_inventory`), gates to
    // [30, 45, 0] + scale 0.8 + translation [0, -1, 0] (`template_fence_gate`), etc.
    // Translation is in MC's "model pixel" units (1/16 of a block).
    struct GuiDisplay {
        glm::vec3 rotation{30.0f, 225.0f, 0.0f};   // degrees, applied as Rx*Ry*Rz
        glm::vec3 translation{0.0f, 0.0f, 0.0f};   // model-pixel units (1/16 of a block)
        glm::vec3 scale{0.625f, 0.625f, 0.625f};
    };

    // Full model for one block type
    struct BlockModel {
        std::string parent;                                    // Parent model (for inheritance)
        std::map<std::string, std::string> textures;          // Texture variable definitions
        std::vector<Element> elements;                         // List of cuboid elements
        GuiDisplay guiDisplay;                                 // display.gui transform for inventory rendering
        // Texture KEYS that MC marked `force_translucent: true` in the modern
        // object-form texture entry: `"all": {"force_translucent": true,
        // "sprite": "minecraft:block/glass"}`. Surfaces that resolve to one of
        // these keys must be routed through the translucent render layer
        // regardless of the texture's actual alpha distribution. The mesher
        // can consult this set when deciding which pass a face belongs in.
        // Empty for the vast majority of blocks (no perf cost).
        std::set<std::string> translucentTextureRefs;

        BlockModel() = default;

        // FIXED: Resolve a texture reference recursively
        std::string ResolveTexture(const std::string& textureRef) const {
            if (textureRef.empty()) {
                return "missingno"; // Empty reference
            }

            // CRITICAL FIX: Handle both "#key" and "key" formats
            std::string cleanRef = textureRef;
            if (cleanRef[0] == '#') {
                cleanRef = cleanRef.substr(1); // Remove '#' prefix
            }

            // Look up in texture map
            auto it = textures.find(cleanRef);
            if (it == textures.end()) {
                // Key not found in texture map
                return "missingno";
            }

            std::string result = it->second;

            // RECURSIVE RESOLUTION: If the result is another reference (starts with '#'), resolve it too
            if (!result.empty() && result[0] == '#') {
                return ResolveTexture(result); // Recursive call
            }

            // CANONICALIZATION: Strip "minecraft:" prefix if present
            if (result.rfind("minecraft:", 0) == 0) {
                result = result.substr(10); // Remove "minecraft:" prefix
            }

            return result;
        }

        // Get all unique texture paths used by this model
        std::vector<std::string> GetAllTexturePaths() const {
            std::set<std::string> uniquePaths; // Use set to avoid duplicates

            for (const auto& element : elements) {
                for (const auto& [dir, face] : element.faces) {
                    std::string path = ResolveTexture(face.textureRef);
                    uniquePaths.insert(path);
                }
            }

            // Convert set to vector
            return std::vector<std::string>(uniquePaths.begin(), uniquePaths.end());
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
        static std::unordered_map<std::string, nlohmann::json> s_rawJsons; // Raw JSON storage
        static BlockModel s_defaultModel;

        // Helper functions - FIXED: Now match the implementation
        static void CreateDefaultModel();
        static BlockModel ResolveModel(const std::string& name);
        static BlockModel ResolveModelRecursive(const std::string& name, int depth);
        static std::string CanonicalizeModelName(const std::string& modelRef);
        static Element ParseElement(const nlohmann::json& elemJson);
    };

} // namespace Game