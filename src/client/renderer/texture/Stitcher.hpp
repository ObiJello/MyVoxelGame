// File: src/client/renderer/texture/Stitcher.hpp
//
// MC net.minecraft.client.renderer.texture.Stitcher (26.3) and the mip-level
// choice of SpriteLoader.stitch, exactly: every sprite gets a slot of its
// size plus `padding` on each side, rounded UP to a multiple of
// 1 << mipLevel (smallestFittingMinTexel), so every slot — and with it every
// sprite's interior, `padding` in from the slot — sits on a 1 << mipLevel
// grid and each mip level's `>> k` is exact. Slots are placed largest first
// (height, then width, then name) into a tree of regions that grows the
// atlas one power of two at a time, alternating axes.
//
// MC's padding is `1 << mipLevel << (anisotropy bits - 1)`: the anisotropy
// widening is left out here — this engine changes anisotropy at run time
// without re-stitching, and the 16 px ring of a level-4 atlas already covers
// what its sampler reaches.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Render {

    class Stitcher {
    public:
        struct Entry {
            std::string name;     // the sprite id, for MC's ordering tie-break
            int width = 0;        // the sprite's own size (one frame of an animation)
            int height = 0;
        };
        // Where a sprite landed: `x`/`y` are the SLOT's corner (MC's
        // TextureAtlasSprite x/y); the sprite's pixels start `padding` in.
        struct Placement {
            size_t entry = 0;     // index into the entries given to Stitch
            int x = 0;
            int y = 0;
        };
        struct Result {
            bool ok = false;
            int width = 0;        // MC storageX / storageY: the atlas size
            int height = 0;
            int mipLevel = 0;     // the level the atlas supports (see ChooseMipLevel)
            int padding = 0;      // 1 << mipLevel
            std::vector<Placement> placements;   // one per entry, in slot-walk order
            std::string failedEntry;             // when !ok: the sprite that did not fit
        };

        // MC SpriteLoader.stitch: the atlas's mip level is `maxMipLevels`
        // unless a sprite's size cannot halve that far — then the smallest
        // power of two dividing every sprite (and no larger than the
        // smallest sprite side) wins, for the WHOLE atlas, with MC's warning.
        static int ChooseMipLevel(const std::vector<Entry>& entries, int maxMipLevels,
                                  const std::string& atlasName);

        // Packs every entry; `maxSize` is the largest texture side allowed.
        static Result Stitch(const std::vector<Entry>& entries, int mipLevel, int maxSize);
    };

} // namespace Render
