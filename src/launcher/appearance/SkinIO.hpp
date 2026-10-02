// File: src/launcher/appearance/SkinIO.hpp
//
// Image plumbing for the Appearance view and the skin editor: PNG files in
// and out (decode through Game::DecodePng, encode through stb_image_write —
// the launcher's only PNG writer), the normalisation a downloaded skin or
// cape needs before the game will take it, base64 for Mojang's textures
// property, and the launcher's GL texture for a skin image.
#pragma once

#include "common/entity/PlayerAppearance.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

typedef unsigned int GLuint;

namespace Launcher::Appearance {

    // Reads and decodes a PNG. False when missing or not an image.
    bool LoadPngFile(const std::string& path, Game::SkinImage& out);
    // Encodes RGBA8 to PNG bytes. Empty on failure.
    std::vector<uint8_t> EncodePng(const Game::SkinImage& image);
    // Encodes and writes (creating the parent directory). Written to a
    // temporary name and renamed, so a reader never sees half a file.
    bool SavePngFile(const std::string& path, const Game::SkinImage& image);
    bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes);

    // A skin as the game samples it: 64x32 legacy skins grown to 64x64 and
    // MC's alpha rules applied (Game::ProcessLegacySkin). False for any
    // other size.
    bool NormalizeSkin(Game::SkinImage& image);

    // A cape as the game takes it — exactly 64x32 (Game::ValidateCapePng).
    //   22x17 (the oldest capes)      → placed at the top-left of a 64x32 sheet
    //   64x64 (legacy alternates)     → the top 64x32 (the cape's region)
    //   k·64 x k·32 (HD capes)        → box-downscaled by k
    //   k·64 x k·64                   → downscaled, then the top half
    // False for anything else.
    bool NormalizeCape(Game::SkinImage& image);

    // Standard (RFC 4648) base64, whitespace ignored. False on bad input.
    bool Base64Decode(std::string_view in, std::vector<uint8_t>& out);

    // A blank (fully transparent) 64x64 image.
    Game::SkinImage BlankSkin();

    // ── GL ──
    // A GL texture holding a skin-type image: nearest filtering, clamped —
    // pixel art, no bleeding between parts. Upload creates on first use and
    // re-specifies the storage when the size changes.
    class GlImage {
    public:
        GlImage() = default;
        ~GlImage();
        GlImage(const GlImage&) = delete;
        GlImage& operator=(const GlImage&) = delete;
        GlImage(GlImage&& o) noexcept;
        GlImage& operator=(GlImage&& o) noexcept;

        void Upload(const Game::SkinImage& image);
        void Release();
        GLuint Id() const { return m_id; }
        int Width() const { return m_w; }
        int Height() const { return m_h; }
        bool Valid() const { return m_id != 0; }

    private:
        GLuint m_id = 0;
        int m_w = 0;
        int m_h = 0;
    };

} // namespace Launcher::Appearance
