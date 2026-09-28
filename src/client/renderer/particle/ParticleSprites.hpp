// File: src/client/renderer/particle/ParticleSprites.hpp
//
// MC's particle atlas (AtlasIds.PARTICLES, atlases/particles.json: every file
// under textures/particle) and the sprite sets ParticleResources binds from
// particles/<type>.json.
//
// Every sprite a particle JSON names is loaded (a resource pack's copy when
// one supplies it), stitched into ONE RGBA texture and addressed by an id;
// each particle type's JSON becomes a SpriteSet — the ordered list of those
// ids — with MC's two pickers:
//
//   Get(set, age, lifetime)  = sprites[age * (n - 1) / lifetime]
//                               (SingleQuadParticle.setSpriteFromAge)
//   Random(set, random)      = sprites[random.nextInt(n)]
//
// One texture means every particle-atlas quad of a frame is one draw per
// layer, as in MC. An animated sprite (a .png.mcmeta beside the texture —
// vibration's) is split into its frames at load; FrameRect picks the frame
// for the client's tick count, as MC's atlas animation does.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "common/core/JavaRandom.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Render {

    class ParticleSpriteAtlas {
    public:
        struct Rect { float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f; };

        struct Sprite {
            Rect     rect;              // frame 0
            uint32_t firstFrame = 0;    // into m_frames, when animated
            uint16_t frameCount = 1;
            uint16_t frameTime = 1;     // ticks per frame (mcmeta frametime)
            // SpriteContents.transparency().hasTranslucent(): some texel is
            // neither fully opaque nor fully clear (Layer.bySprite).
            bool     translucent = false;
        };

        // (Re)build from assets/particles/*.json and textures/particle.
        // Main thread; needs the render backend. Returns false when not a
        // single sprite loaded (the texture stays invalid then).
        bool Load();
        void Destroy();

        TextureHandle Texture() const { return m_texture; }

        // The sprite set of particle type `typeName` ("smoke" or
        // "minecraft:smoke"); -1 when the type has no JSON / no sprites.
        int SetIndex(std::string_view typeName) const;
        int SetSize(int set) const;
        // MC SpriteSet.get(index, max) / get(random) / first().
        int Get(int set, int age, int lifetime) const;
        int Random(int set, Game::JavaRandom& random) const;
        int First(int set) const;

        // A sprite by its texture name ("glint", "generic_3"); -1 if absent.
        int SpriteIndex(std::string_view textureName) const;

        const Sprite& GetSprite(int id) const { return m_sprites[static_cast<size_t>(id)]; }
        bool Valid(int id) const { return id >= 0 && static_cast<size_t>(id) < m_sprites.size(); }

        // The sprite's rectangle this tick (its animation frame).
        Rect FrameRect(int id, int64_t clientTicks) const;

    private:
        struct Image {
            std::string name;
            int width = 0, height = 0;         // one frame
            std::vector<uint8_t> pixels;       // all frames, stacked vertically
            int frames = 1;
            int frameTime = 1;
            std::vector<int> frameOrder;       // mcmeta "frames" (indices), empty = 0..frames-1
        };

        TextureHandle m_texture = INVALID_TEXTURE;
        std::vector<Sprite> m_sprites;
        std::vector<Rect>   m_frames;
        std::unordered_map<std::string, int> m_spriteByName;
        std::vector<std::vector<int>> m_sets;
        std::unordered_map<std::string, int> m_setByType;   // "smoke" (no namespace)
    };

} // namespace Render
