#pragma once
#include <stdexcept>
#include <string>
#include <vector>
#include "stb_image.h"

// Image-loading adapter only. The archived UIFontData/UIBitmapFont code
// computes the original glyph map, advances, baseline and integer scaling.
class BufferedImage {
    std::vector<int> pixels_;
public:
    explicit BufferedImage(const std::wstring& resource) {
        std::string path = "source_full/Minecraft.Client/Common/res";
        for (wchar_t c : resource) path += char(c); // The two font paths are ASCII.
        int width=0, height=0, channels=0;
        auto* rgba=stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!rgba) throw std::runtime_error("Cannot read original font: " + path);
        pixels_.resize(size_t(width)*height);
        for (size_t i=0; i<pixels_.size(); ++i)
            pixels_[i]=int((unsigned(rgba[i*4+3])<<24)|unsigned(rgba[i*4]));
        stbi_image_free(rgba);
    }
    int* getData() { return pixels_.data(); }
};
