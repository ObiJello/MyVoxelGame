// File: src/client/renderer/gui/BlockIcon.hpp
//
// The app icon drawn from a block, on the CPU — shared by the game (its Dock /
// window icon when --icon names a block or `random`) and the launcher (the
// Settings → App icon picker's thumbnails). TNT, the default, also ships
// pre-rendered (tools/make_game_icon.py → src/platform/icon/): the bundle /
// exe icon, and what the game shows without --icon.
//
// A block qualifies when it is a FULL block: its item definition is a plain
// `minecraft:model` pointing at a block model whose elements are all whole
// cubes (from [0,0,0] to [16,16,16], unrotated), and whose three faces the
// icon shows (up, north, west) are fully opaque in their base layer — no
// glass, leaves or slabs. Overlay elements (the grass block's tinted side) are
// drawn over the base as the model layers them; tints come from the item
// definition (`constant`, and `grass` off the colormap) as MC's GUI applies
// them.
//
// The icon follows Minecraft's own game icon (icons/minecraft.icns, the grass
// block): no background, a true isometric cube filling the full height of the
// square (86.6 % as wide), the top face at full brightness, the left face a
// little dimmer, the right face in deep shadow, and a thin light rim along
// the top face's two front edges. tools/make_game_icon.py draws the
// pre-rendered TNT the same way; the two share their constants.
//
// Everything reads the engine's own assets directory (`assets/` in the game
// bundle or next to the executable) and nothing else — no resource packs, so
// the launcher's thumbnail is the icon the game will show.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Render::BlockIcon {

    // The --icon values beside a block id: the default (shown pre-rendered,
    // and passed as no flag at all) and a new random full block per launch.
    inline constexpr const char* kDefaultBlock = "tnt";
    inline constexpr const char* kRandom = "random";

    // An RGBA8 image, rows top to bottom, straight (not premultiplied) alpha.
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;

        bool Valid() const {
            return width > 0 && height > 0 &&
                   rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        }
    };

    // One face of one model element as the icon draws it.
    struct Face {
        bool  present = false;
        Image texture;            // the sprite (frame 0 of an animated strip)
        float uv[4] = {0.0f, 0.0f, 16.0f, 16.0f};   // model "uv" (u0, v0, u1, v1)
        int   rotation = 0;       // model "rotation", degrees clockwise
        uint32_t tint = 0xFFFFFFFFu;   // ARGB multiplier (white = untinted)
    };

    // A whole-cube element's three visible faces.
    struct Element {
        Face up;
        Face north;   // the icon's left face
        Face west;    // the icon's right face
    };

    // A full block, resolved and ready to draw.
    struct Model {
        std::string id;           // "grass_block"
        std::string name;         // "Grass Block" (en_us), or the id when unnamed
        std::vector<Element> elements;
    };

    // The block `id` ("stone", "minecraft:stone") if it is a full block;
    // nullopt for anything else or anything that fails to load.
    std::optional<Model> LoadFullBlock(const std::filesystem::path& assetsDir, const std::string& id);

    // Every full block, sorted by display name.
    std::vector<Model> LoadAllFullBlocks(const std::filesystem::path& assetsDir);

    // A uniformly random full block (items tried in a shuffled order, the
    // first full block wins — cheap enough for every launch). nullopt when
    // the directory holds none.
    std::optional<Model> LoadRandomFullBlock(const std::filesystem::path& assetsDir, uint64_t seed);

    // The icon at `size` x `size` pixels.
    Image Render(const Model& model, int size);

} // namespace Render::BlockIcon
