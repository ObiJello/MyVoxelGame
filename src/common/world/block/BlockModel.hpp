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
        // Parsed form of `cullface`: the FaceDir whose NEIGHBOUR decides whether
        // this face is dropped, or -1 for "never cull". MC only culls a quad
        // that declares a cullface (BlockElementFace.cullForDirection is
        // @Nullable), and the direction it names need not match the face's own
        // normal — RotateModel rewrites it precisely because a turned model
        // points its cullface somewhere else.
        //
        // Kept as a parsed enum because the mesher tests it once per face per
        // block, and a string compare chain there is on the hottest path in the
        // engine. Always assign through SetCullface() so the two stay in sync.
        int8_t cullfaceDir = -1;

        void SetCullface(const std::string& cull) {
            cullface = cull;
            cullfaceDir = cull.empty()
                              ? int8_t(-1)
                              : static_cast<int8_t>(ParseFaceDir(cull));
        }
        // MC's per-face `"rotation"` (0/90/180/270), which rotates the texture
        // on this face only. It is NOT a geometry transform — MC implements it
        // as a permutation of which UV corner each vertex reads
        // (BlockElementFace.getU + Quadrant.rotateVertexIndex). Blocks that need
        // it include every glazed terracotta (template_glazed_terracotta.json
        // rotates its four sides 90/270/0/180 to make the pattern continuous).
        int uvRotation = 0;

        FaceDef() = default;
        FaceDef(const glm::vec4& uvCoords, const std::string& texture, int tint = -1,
                const std::string& cull = "", int rotation = 0)
            : uv(uvCoords), textureRef(texture), tintIndex(tint)
            , uvRotation(rotation) { SetCullface(cull); }
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

        bool IsIdentity() const { return axis == 0 || angle == 0.0f; }
    };

    // MC FaceBakery.rotateVertexBy + BlockElementRotation.transform: rotate a
    // point about the element's own origin.
    //
    // Unlike the whole-model blockstate rotation (BlockModelRegistry::RotateModel)
    // this is an ARBITRARY angle, so it cannot be baked back into an
    // axis-aligned from/to pair — it has to be applied per vertex at mesh time.
    // Skipping it leaves elements at their pre-rotation coordinates, which for
    // fanned-out geometry like the flowerbed stems means far outside the block:
    // `flowerbed_3` authors a stem at x≈17.65 and relies on a -45° turn about
    // the corner to bring it back in next to its petals.
    //
    // `point` and `rot.origin` are both in MC model space (0..16). `scale`
    // lets callers work in block space instead by passing 1/16.
    glm::vec3 ApplyElementRotation(const glm::vec3& point, const ElementRotation& rot,
                                   float scale = 1.0f);

    // MC's fixed per-face brightness multiplier — the "fake directional light"
    // that makes a cube's top read brighter than its sides with no light source
    // involved (ClientLevel.getShade / ModelBlockRenderer). Baked into vertex
    // colour at mesh-build time on every path that draws a block model, so a
    // block looks the same in the world, in your hand, and lying on the ground.
    //
    // Gated per element by `Element::shade` below: models that opt out (crossed
    // planes like saplings and torches) would otherwise show their two quads at
    // visibly different brightness.
    //
    // CANONICAL. Mesher::GetDirectionalShade delegates here — do not fork a
    // second copy of these numbers, or terrain and items will disagree.
    constexpr float DirectionalShade(FaceDir dir) {
        switch (dir) {
            case FaceDir::Up:    return 1.0f;   // +Y
            case FaceDir::Down:  return 0.5f;   // -Y
            case FaceDir::North:                // -Z
            case FaceDir::South: return 0.8f;   // +Z
            case FaceDir::West:                 // -X
            case FaceDir::East:  return 0.6f;   // +X
        }
        return 1.0f;
    }

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
        // MC BlockModel.hasAmbientOcclusion — a nullable Boolean resolved up the
        // parent chain, defaulting to true. When false, AO is skipped for the
        // WHOLE model; it is a model-level flag, unlike `shade` which is
        // per-element. ModelBlockRenderer.java:42:
        //   useAO = Minecraft.useAmbientOcclusion()
        //           && state.getLightEmission() == 0
        //           && parts.getFirst().useAmbientOcclusion();
        //
        // Vanilla turns it off on every cross-shaped plant parent (block/cross,
        // block/tinted_cross, block/crop, …). Ignoring it darkens a tuft of
        // grass against the very block it stands on, which vanilla never does.
        //
        // The `getLightEmission() == 0` clause is NOT implemented — this engine
        // carries no per-block light-emission data — so full-bright blocks that
        // vanilla exempts from AO (glowstone, sea lantern, …) still receive it.
        bool ambientOcclusion = true;
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

        // Install a model under a name that has no file on disk. Used for the
        // rotated variants synthesised from blockstate `x`/`y` values, which
        // MC produces at bake time rather than shipping as separate files.
        static void RegisterModel(const std::string& name, BlockModel model);

        // Rotate a resolved model by whole quarter turns about X then Y, in
        // that order — the same composition order MC's BlockModelRotation uses
        // for a blockstate variant's `"x"` and `"y"` fields.
        //
        // Rotation is applied ONCE here (at load), producing a model whose
        // geometry, face directions, cullfaces and per-face UV rotations are
        // already in world space. That mirrors MC baking rotated BakedQuads per
        // blockstate: the mesher stays completely orientation-agnostic, and in
        // particular neighbour occlusion keeps working because cullfaces come
        // out as rotated world directions (MC: SimpleUnbakedGeometry rotates
        // them with Direction.rotate for exactly this reason).
        //
        // `uvLock` is the blockstate variant's `"uvlock"` field. MC applies a
        // variant's rotation to the GEOMETRY always, and to the UVs only when
        // uvlock is false — a uvlocked face keeps its texture aligned to the
        // world, which is what stops a stair's plank grain or a sandstone top
        // from spinning as the stair is turned. The mesher here derives UVs
        // from fixed world-space axes, so world-aligned is what it does with
        // no correction at all: uvLock=true means "skip the uvRotation solve
        // below", and uvLock=false means "apply it".
        static BlockModel RotateModel(const BlockModel& src, int xQuarterTurns,
                                      int yQuarterTurns, bool uvLock = false);

        // Concatenate several resolved models into one.
        //
        // MC's `multipart` blockstates are ADDITIVE: every entry whose `when`
        // matches contributes its model's quads to the same block, which is how
        // a 3-segment leaf litter clump is drawn as leaf_litter_2 (a half) PLUS
        // leaf_litter_3 (a quarter). Since a state here resolves to exactly one
        // model name, the matching parts are merged once at load and registered
        // under a derived name — the same trick already used for rotations.
        //
        // Parts must already be rotated; this only unions geometry. Texture
        // maps are merged with earlier parts winning, and elements are
        // self-contained after resolution so no reference can dangle.
        static BlockModel MergeModels(const std::vector<const BlockModel*>& parts);

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