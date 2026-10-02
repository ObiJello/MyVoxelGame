// File: src/client/renderer/texture/PalettedPermutations.cpp
#include "PalettedPermutations.hpp"

#include "../../../ext/stb_image/stb_image.h"

#include <cstring>
#include <unordered_map>

namespace Render::PalettedPermutations {

    namespace {
        bool LoadRgba(const std::string& file, int& w, int& h, std::vector<unsigned char>& out) {
            int channels = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(file.c_str(), &w, &h, &channels, STBI_rgb_alpha);
            if (!pixels) return false;
            out.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            stbi_image_free(pixels);
            return true;
        }
    } // namespace

    std::vector<uint32_t> LoadPalette(const std::string& file) {
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
        if (!LoadRgba(file, w, h, rgba)) return {};
        std::vector<uint32_t> out;
        out.reserve(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            out.push_back((static_cast<uint32_t>(rgba[i + 3]) << 24) | (static_cast<uint32_t>(rgba[i]) << 16) |
                          (static_cast<uint32_t>(rgba[i + 1]) << 8) | static_cast<uint32_t>(rgba[i + 2]));
        }
        return out;
    }

    bool Apply(std::vector<unsigned char>& rgba, const std::vector<uint32_t>& keyPalette,
               const std::vector<uint32_t>& targetPalette) {
        if (keyPalette.size() != targetPalette.size()) return false;
        // PaletteMapping.create: opaque(key) -> target, for every key entry
        // with alpha.
        std::unordered_map<uint32_t, uint32_t> mapping;
        for (size_t i = 0; i < keyPalette.size(); ++i) {
            if ((keyPalette[i] >> 24) != 0) mapping.emplace(keyPalette[i] | 0xFF000000u, targetPalette[i]);
        }
        // PaletteMapping.apply.
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            const uint32_t baseAlpha = rgba[i + 3];
            if (baseAlpha == 0) continue;
            const uint32_t baseRgb = 0xFF000000u | (static_cast<uint32_t>(rgba[i]) << 16) |
                                     (static_cast<uint32_t>(rgba[i + 1]) << 8) | rgba[i + 2];
            const auto it = mapping.find(baseRgb);
            const uint32_t target = it != mapping.end() ? it->second : baseRgb;
            const uint32_t valueAlpha = target >> 24;
            rgba[i]     = static_cast<unsigned char>((target >> 16) & 0xFF);
            rgba[i + 1] = static_cast<unsigned char>((target >> 8) & 0xFF);
            rgba[i + 2] = static_cast<unsigned char>(target & 0xFF);
            rgba[i + 3] = static_cast<unsigned char>(baseAlpha * valueAlpha / 255u);
        }
        return true;
    }

    bool Build(const std::string& file, const std::string& keyPaletteFile, const std::string& targetPaletteFile,
               int& width, int& height, std::vector<unsigned char>& rgba) {
        const std::vector<uint32_t> key = LoadPalette(keyPaletteFile);
        const std::vector<uint32_t> target = LoadPalette(targetPaletteFile);
        if (key.empty() || target.empty()) return false;
        if (!LoadRgba(file, width, height, rgba)) return false;
        return Apply(rgba, key, target);
    }

} // namespace Render::PalettedPermutations
