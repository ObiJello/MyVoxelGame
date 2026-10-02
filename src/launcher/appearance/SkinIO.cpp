// File: src/launcher/appearance/SkinIO.cpp
#include "SkinIO.hpp"
#include "common/core/Log.hpp"

#include <glad/glad.h>

// The launcher's one stb_image_write implementation. The game target defines
// it in AtlasBuilder.cpp, which the launcher does not compile.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image_write.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace Launcher::Appearance {

    namespace {
        void AppendBytes(void* context, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            const auto* bytes = static_cast<const uint8_t*>(data);
            out->insert(out->end(), bytes, bytes + size);
        }

        // Box filter by an integer factor (HD capes).
        Game::SkinImage Downscale(const Game::SkinImage& src, int k) {
            Game::SkinImage out;
            out.width = src.width / k;
            out.height = src.height / k;
            out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4u, 0);
            for (int y = 0; y < out.height; ++y) {
                for (int x = 0; x < out.width; ++x) {
                    unsigned sum[4] = {0, 0, 0, 0};
                    for (int dy = 0; dy < k; ++dy) {
                        for (int dx = 0; dx < k; ++dx) {
                            const uint8_t* p = src.PixelPtr(x * k + dx, y * k + dy);
                            for (int c = 0; c < 4; ++c) sum[c] += p[c];
                        }
                    }
                    uint8_t* d = out.PixelPtr(x, y);
                    for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / static_cast<unsigned>(k * k));
                }
            }
            return out;
        }

        Game::SkinImage Crop(const Game::SkinImage& src, int w, int h) {
            Game::SkinImage out;
            out.width = w;
            out.height = h;
            out.rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
            for (int y = 0; y < h && y < src.height; ++y) {
                const int rowW = std::min(w, src.width);
                std::memcpy(out.PixelPtr(0, y), src.PixelPtr(0, y), static_cast<size_t>(rowW) * 4u);
            }
            return out;
        }
    } // namespace

    bool LoadPngFile(const std::string& path, Game::SkinImage& out) {
        std::vector<uint8_t> bytes;
        if (!Game::ReadFileBytes(path, bytes, 16u * 1024u * 1024u)) return false;
        return Game::DecodePng(bytes, out);
    }

    std::vector<uint8_t> EncodePng(const Game::SkinImage& image) {
        std::vector<uint8_t> out;
        if (!image.Valid()) return out;
        if (!stbi_write_png_to_func(AppendBytes, &out, image.width, image.height, 4,
                                    image.rgba.data(), image.width * 4)) {
            out.clear();
        }
        return out;
    }

    bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
        std::error_code ec;
        const std::filesystem::path target(path);
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
        const std::filesystem::path tmp = target.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) return false;
        }
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            // Windows refuses to rename over an existing file on some setups.
            std::filesystem::remove(target, ec);
            std::filesystem::rename(tmp, target, ec);
        }
        if (ec) {
            Log::Warning("[Appearance] could not write %s: %s", path.c_str(), ec.message().c_str());
            return false;
        }
        return true;
    }

    bool SavePngFile(const std::string& path, const Game::SkinImage& image) {
        const std::vector<uint8_t> png = EncodePng(image);
        if (png.empty()) return false;
        return WriteFileBytes(path, png);
    }

    bool NormalizeSkin(Game::SkinImage& image) {
        return Game::ProcessLegacySkin(image);
    }

    bool NormalizeCape(Game::SkinImage& image) {
        if (!image.Valid()) return false;
        if (image.width == 64 && image.height == 32) return true;
        if (image.width == 22 && image.height == 17) {
            image = Crop(image, 64, 32);
            return true;
        }
        if (image.width % 64 != 0) return false;
        const int k = image.width / 64;
        if (image.height == 32 * k) {
            if (k > 1) image = Downscale(image, k);
            return true;
        }
        if (image.height == 64 * k) {
            if (k > 1) image = Downscale(image, k);
            image = Crop(image, 64, 32);
            return true;
        }
        return false;
    }

    bool Base64Decode(std::string_view in, std::vector<uint8_t>& out) {
        out.clear();
        out.reserve(in.size() * 3 / 4);
        uint32_t acc = 0;
        int bits = 0;
        int pad = 0;
        for (const char c : in) {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+' || c == '-') v = 62;
            else if (c == '/' || c == '_') v = 63;
            else if (c == '=') { ++pad; continue; }
            else return false;
            if (pad) return false;   // data after padding
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
            }
        }
        return pad <= 2;
    }

    Game::SkinImage BlankSkin() {
        Game::SkinImage img;
        img.width = 64;
        img.height = 64;
        img.rgba.assign(64u * 64u * 4u, 0);
        return img;
    }

    // ── GlImage ──

    GlImage::~GlImage() { Release(); }

    GlImage::GlImage(GlImage&& o) noexcept : m_id(o.m_id), m_w(o.m_w), m_h(o.m_h) {
        o.m_id = 0;
        o.m_w = o.m_h = 0;
    }

    GlImage& GlImage::operator=(GlImage&& o) noexcept {
        if (this != &o) {
            Release();
            m_id = std::exchange(o.m_id, 0u);
            m_w = std::exchange(o.m_w, 0);
            m_h = std::exchange(o.m_h, 0);
        }
        return *this;
    }

    void GlImage::Upload(const Game::SkinImage& image) {
        if (!image.Valid()) return;
        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        if (m_id == 0) {
            glGenTextures(1, &m_id);
            glBindTexture(GL_TEXTURE_2D, m_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, m_id);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (image.width != m_w || image.height != m_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
            m_w = image.width;
            m_h = image.height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width, image.height,
                            GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    }

    void GlImage::Release() {
        if (m_id != 0) {
            glDeleteTextures(1, &m_id);
            m_id = 0;
        }
        m_w = m_h = 0;
    }

} // namespace Launcher::Appearance
