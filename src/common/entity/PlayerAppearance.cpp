// File: src/common/entity/PlayerAppearance.cpp
#include "PlayerAppearance.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace Game {

    namespace {

        std::string Lower(std::string_view s) {
            std::string out;
            out.reserve(s.size());
            for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            return out;
        }

        constexpr uint8_t kColorCount = static_cast<uint8_t>(PlayerColorId::Count);

        constexpr std::string_view kPaintHeader = "obeycraft-stickfigure 1";
        // Versions 2 (paint + the retired voxel sculpt) and 3 (paint + a
        // pixel drawing) read with the same line parser — the drawing line
        // says its own encoding; the launcher writes version 4 (paint + a
        // stroke drawing).
        constexpr std::string_view kV2Header    = "obeycraft-stickfigure 2";
        constexpr std::string_view kV3Header    = "obeycraft-stickfigure 3";
        constexpr std::string_view kFileHeader  = "obeycraft-stickfigure 4";

        std::string_view Trim(std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return s;
        }

        int HexValue(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // MC NativeImage.copyRect(startX, startY, offsetX, offsetY, sizeX,
        // sizeY, swapX, swapY) on one image.
        void CopyRect(SkinImage& img, int startX, int startY, int offsetX, int offsetY,
                      int sizeX, int sizeY, bool swapX, bool swapY) {
            for (int y = 0; y < sizeY; ++y) {
                for (int x = 0; x < sizeX; ++x) {
                    const int dx = swapX ? sizeX - 1 - x : x;
                    const int dy = swapY ? sizeY - 1 - y : y;
                    const uint8_t* src = img.PixelPtr(startX + x, startY + y);
                    uint8_t px[4];
                    std::memcpy(px, src, 4);
                    std::memcpy(img.PixelPtr(startX + offsetX + dx, startY + offsetY + dy), px, 4);
                }
            }
        }

        void FillRect(SkinImage& img, int x0, int y0, int w, int h, uint32_t rgba) {
            const uint8_t px[4] = { static_cast<uint8_t>(rgba >> 24), static_cast<uint8_t>(rgba >> 16),
                                    static_cast<uint8_t>(rgba >> 8), static_cast<uint8_t>(rgba) };
            for (int y = y0; y < y0 + h; ++y) {
                for (int x = x0; x < x0 + w; ++x) std::memcpy(img.PixelPtr(x, y), px, 4);
            }
        }

        // MC SkinTextureDownloader.setNoAlpha.
        void SetNoAlpha(SkinImage& img, int x0, int y0, int x1, int y1) {
            for (int x = x0; x < x1; ++x) {
                for (int y = y0; y < y1; ++y) img.PixelPtr(x, y)[3] = 255;
            }
        }

        // MC SkinTextureDownloader.doNotchTransparencyHack: a legacy hat with
        // no transparent pixel at all was painted as a solid box by old
        // editors — clear it.
        void NotchTransparencyHack(SkinImage& img, int x0, int y0, int x1, int y1) {
            for (int x = x0; x < x1; ++x) {
                for (int y = y0; y < y1; ++y) {
                    if (img.PixelPtr(x, y)[3] < 128) return;
                }
            }
            for (int x = x0; x < x1; ++x) {
                for (int y = y0; y < y1; ++y) img.PixelPtr(x, y)[3] = 0;
            }
        }

        bool ProbePng(const std::vector<uint8_t>& png, size_t maxBytes, int& w, int& h, std::string* why) {
            if (png.empty()) {
                if (why) *why = "empty";
                return false;
            }
            if (png.size() > maxBytes) {
                if (why) *why = "larger than " + std::to_string(maxBytes / 1024) + " KB";
                return false;
            }
            int channels = 0;
            if (!stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels)) {
                if (why) *why = "not a readable image";
                return false;
            }
            // stbi_info only reads the header: decode for real, so a file
            // with a valid header and a corrupt body is refused here rather
            // than on every client that receives it.
            SkinImage decoded;
            if (!DecodePng(png, decoded)) {
                if (why) *why = "corrupt image data";
                return false;
            }
            return true;
        }

    } // namespace

    const char* AppearanceModeSlug(AppearanceMode mode) {
        return mode == AppearanceMode::Skin ? "skin" : "stick";
    }

    std::optional<AppearanceMode> ParseAppearanceMode(std::string_view slug) {
        const std::string s = Lower(slug);
        if (s == "stick" || s == "stickfigure" || s == "stick-figure") return AppearanceMode::StickFigure;
        if (s == "skin" || s == "minecraft") return AppearanceMode::Skin;
        return std::nullopt;
    }

    const char* SkinModelSlug(SkinModel model) {
        return model == SkinModel::Slim ? "slim" : "classic";
    }

    std::optional<SkinModel> ParseSkinModel(std::string_view slug) {
        const std::string s = Lower(slug);
        if (s == "classic" || s == "wide" || s == "default" || s == "steve") return SkinModel::Classic;
        if (s == "slim" || s == "alex") return SkinModel::Slim;
        return std::nullopt;
    }

    // ── StickFigurePaint ────────────────────────────────────────────────────

    StickFigurePaint::Part StickFigurePaint::PartOf(int cell) {
        int first = 0;
        for (int i = 0; i < kPartCount; ++i) {
            if (cell < first + kCellsPerPart[i]) return static_cast<Part>(i);
            first += kCellsPerPart[i];
        }
        return Part::BackOfHead;
    }

    const char* StickFigurePaint::PartName(Part p) {
        switch (p) {
            case Part::Torso:      return "Body";
            case Part::LeftArm:    return "Left arm";
            case Part::RightArm:   return "Right arm";
            case Part::LeftLeg:    return "Left leg";
            case Part::RightLeg:   return "Right leg";
            case Part::HeadRing:   return "Head outline";
            case Part::Eyes:       return "Eyes";
            case Part::Smile:      return "Smile";
            case Part::BackOfHead: return "Back of head";
            default:               return "";
        }
    }

    StickFigurePaint StickFigurePaint::Uniform(PlayerColorId color) {
        StickFigurePaint p;
        p.cells.fill(static_cast<uint8_t>(color) < kColorCount ? static_cast<uint8_t>(color) : 0);
        return p;
    }

    PlayerColorId StickFigurePaint::At(int cell) const {
        if (cell < 0 || cell >= kCellCount) return PlayerColorId::Default;
        const uint8_t v = cells[static_cast<size_t>(cell)];
        return v < kColorCount ? static_cast<PlayerColorId>(v) : PlayerColorId::Default;
    }

    void StickFigurePaint::Set(int cell, PlayerColorId color) {
        if (cell < 0 || cell >= kCellCount) return;
        const auto v = static_cast<uint8_t>(color);
        cells[static_cast<size_t>(cell)] = v < kColorCount ? v : 0;
    }

    void StickFigurePaint::Fill(Part p, PlayerColorId color) {
        const int first = FirstCell(p);
        for (int i = 0; i < CellCount(p); ++i) Set(first + i, color);
    }

    bool StickFigurePaint::IsUniform() const {
        for (const uint8_t c : cells) {
            if (c != cells[0]) return false;
        }
        return true;
    }

    void StickFigurePaint::Sanitize() {
        for (uint8_t& c : cells) {
            if (c >= kColorCount) c = 0;
        }
    }

    std::string StickFigurePaint::ToText() const {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(kPaintHeader);
        out.push_back('\n');
        for (const uint8_t c : cells) out.push_back(kHex[c & 0x0F]);
        out.push_back('\n');
        return out;
    }

    std::optional<StickFigurePaint> StickFigurePaint::FromText(std::string_view text) {
        // Header line, then the cells; whitespace between digits is allowed
        // (a hand-edited file may wrap them).
        const size_t nl = text.find('\n');
        std::string_view header = text.substr(0, nl);
        while (!header.empty() && (header.back() == '\r' || header.back() == ' ')) header.remove_suffix(1);
        if (header != kPaintHeader) return std::nullopt;
        if (nl == std::string_view::npos) return std::nullopt;
        StickFigurePaint paint;
        int n = 0;
        for (const char c : text.substr(nl + 1)) {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            const int v = HexValue(c);
            if (v < 0 || n >= kCellCount) return std::nullopt;
            paint.cells[static_cast<size_t>(n++)] = static_cast<uint8_t>(v);
        }
        if (n != kCellCount) return std::nullopt;
        paint.Sanitize();
        return paint;
    }

    // ── StickFigureFile ─────────────────────────────────────────────────────

    std::string StickFigureFile::ToText() const {
        std::string out(kFileHeader);
        out.push_back('\n');
        if (hasPaint) {
            // The paint line is the version-1 body: the cells after its header.
            const std::string v1 = paint.ToText();
            out += "paint ";
            out += Trim(std::string_view(v1).substr(kPaintHeader.size()));
            out.push_back('\n');
        }
        if (!drawing.Empty()) {
            out += "drawing ";
            out += drawing.ToHex();
            out.push_back('\n');
        }
        return out;
    }

    std::optional<StickFigureFile> StickFigureFile::FromText(std::string_view text, std::string* why) {
        const size_t nl = text.find('\n');
        const std::string_view header = Trim(text.substr(0, nl));
        if (header == kPaintHeader) {
            auto paint = StickFigurePaint::FromText(text);
            if (!paint) {
                if (why) *why = "not a painted figure";
                return std::nullopt;
            }
            StickFigureFile file;
            file.hasPaint = true;
            file.paint = *paint;
            return file;
        }
        if (header != kFileHeader && header != kV3Header && header != kV2Header) {
            if (why) *why = "not a stick-figure file";
            return std::nullopt;
        }
        StickFigureFile file;
        std::string_view rest = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        while (!rest.empty()) {
            const size_t end = rest.find('\n');
            const std::string_view line = Trim(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
            const size_t space = line.find(' ');
            const std::string_view key = line.substr(0, space);
            const std::string_view value = space == std::string_view::npos ? std::string_view()
                                                                            : Trim(line.substr(space + 1));
            if (key == "paint") {
                std::string v1(kPaintHeader);
                v1.push_back('\n');
                v1.append(value);
                if (auto paint = StickFigurePaint::FromText(v1)) {
                    file.hasPaint = true;
                    file.paint = *paint;
                } else if (why) {
                    *why = "the paint line is malformed";
                }
            } else if (key == "drawing") {
                std::string reason;
                if (auto drawing = StickFigureDrawing::FromHex(std::string(value), &reason)) {
                    file.drawing = std::move(*drawing);
                } else if (why) {
                    *why = "drawing dropped (" + reason + ")";
                }
            } else if (key == "sculpt") {
                // Version 2's voxels built onto the figure: retired, so the
                // figure loads without them.
                if (why && !value.empty()) *why = "the old 3D voxels are no longer worn; skipped";
            }
            // Anything else: a newer launcher's line, skipped.
        }
        return file;
    }

    // ── Images ──────────────────────────────────────────────────────────────

    bool DecodePng(const uint8_t* data, size_t size, SkinImage& out) {
        out = SkinImage{};
        if (!data || size == 0 || size > 16u * 1024u * 1024u) return false;
        int w = 0, h = 0, channels = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels,
                                                      STBI_rgb_alpha);
        if (!pixels) return false;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
            stbi_image_free(pixels);
            return false;
        }
        out.width = w;
        out.height = h;
        out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        stbi_image_free(pixels);
        return true;
    }

    bool ProcessLegacySkin(SkinImage& image) {
        if (!image.Valid() || image.width != 64 || (image.height != 32 && image.height != 64)) return false;
        const bool isLegacy = image.height == 32;
        if (isLegacy) {
            SkinImage grown;
            grown.width = 64;
            grown.height = 64;
            grown.rgba.assign(64u * 64u * 4u, 0);
            std::memcpy(grown.rgba.data(), image.rgba.data(), image.rgba.size());
            image = std::move(grown);
            FillRect(image, 0, 32, 64, 32, 0u);
            CopyRect(image, 4, 16, 16, 32, 4, 4, true, false);
            CopyRect(image, 8, 16, 16, 32, 4, 4, true, false);
            CopyRect(image, 0, 20, 24, 32, 4, 12, true, false);
            CopyRect(image, 4, 20, 16, 32, 4, 12, true, false);
            CopyRect(image, 8, 20, 8, 32, 4, 12, true, false);
            CopyRect(image, 12, 20, 16, 32, 4, 12, true, false);
            CopyRect(image, 44, 16, -8, 32, 4, 4, true, false);
            CopyRect(image, 48, 16, -8, 32, 4, 4, true, false);
            CopyRect(image, 40, 20, 0, 32, 4, 12, true, false);
            CopyRect(image, 44, 20, -8, 32, 4, 12, true, false);
            CopyRect(image, 48, 20, -16, 32, 4, 12, true, false);
            CopyRect(image, 52, 20, -8, 32, 4, 12, true, false);
        }
        SetNoAlpha(image, 0, 0, 32, 16);
        if (isLegacy) NotchTransparencyHack(image, 32, 0, 64, 32);
        SetNoAlpha(image, 0, 16, 64, 32);
        SetNoAlpha(image, 16, 48, 48, 64);
        return true;
    }

    bool ValidateSkinPng(const std::vector<uint8_t>& png, std::string* why) {
        int w = 0, h = 0;
        if (!ProbePng(png, kMaxSkinPngBytes, w, h, why)) return false;
        if (w != 64 || (h != 64 && h != 32)) {
            if (why) *why = "skin is " + std::to_string(w) + "x" + std::to_string(h) + ", not 64x64 or 64x32";
            return false;
        }
        return true;
    }

    bool ValidateCapePng(const std::vector<uint8_t>& png, std::string* why) {
        int w = 0, h = 0;
        if (!ProbePng(png, kMaxCapePngBytes, w, h, why)) return false;
        if (w != 64 || h != 32) {
            if (why) *why = "cape is " + std::to_string(w) + "x" + std::to_string(h) + ", not 64x32";
            return false;
        }
        return true;
    }

    const char* DefaultSkinAssetPath(SkinModel model) {
        return model == SkinModel::Slim ? "assets/textures/entity/player/slim/alex.png"
                                        : "assets/textures/entity/player/wide/steve.png";
    }

    // ── PlayerAppearance ────────────────────────────────────────────────────

    void PlayerAppearance::Sanitize(std::vector<std::string>* log) {
        if (mode != AppearanceMode::StickFigure && mode != AppearanceMode::Skin) {
            mode = AppearanceMode::StickFigure;
            if (log) log->push_back("unknown appearance mode, using the stick figure");
        }
        if (model != SkinModel::Classic && model != SkinModel::Slim) {
            model = SkinModel::Classic;
            if (log) log->push_back("unknown skin model, using classic");
        }
        modelParts &= ModelPartBits::All;
        std::string why;
        if (!skinPng.empty() && !ValidateSkinPng(skinPng, &why)) {
            skinPng.clear();
            if (log) log->push_back("skin dropped (" + why + "), using the default skin");
        }
        if (!capePng.empty() && !ValidateCapePng(capePng, &why)) {
            capePng.clear();
            if (log) log->push_back("cape dropped (" + why + ")");
        }
        if (hasPaint) paint.Sanitize();
        if (!drawing.Empty()) {
            if (IsSkin()) {
                // A drawing replaces the stick figure; a skin has no use for it.
                drawing.Clear();
            } else if (!drawing.Validate(&why)) {
                drawing.Clear();
                if (log) log->push_back("drawn figure dropped (" + why + ")");
            }
        }
    }

    bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes) {
        out.clear();
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return false;
        const std::streamoff size = in.tellg();
        if (size < 0 || static_cast<size_t>(size) > maxBytes) return false;
        out.resize(static_cast<size_t>(size));
        in.seekg(0);
        if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
            out.clear();
            return false;
        }
        return true;
    }

    PlayerAppearance LoadAppearanceFromArgs(const AppearanceArgs& args, std::vector<std::string>* log) {
        PlayerAppearance a;
        if (!args.mode.empty()) {
            if (const auto mode = ParseAppearanceMode(args.mode)) {
                a.mode = *mode;
            } else if (log) {
                log->push_back("--skin-mode '" + args.mode + "' is not stick or skin");
            }
        } else if (!args.skinPath.empty() || !args.model.empty()) {
            // A skin without a mode means the skin.
            a.mode = AppearanceMode::Skin;
        }
        if (!args.model.empty()) {
            if (const auto model = ParseSkinModel(args.model)) {
                a.model = *model;
            } else if (log) {
                log->push_back("--skin-model '" + args.model + "' is not classic or slim");
            }
        }
        if (!args.skinPath.empty()) {
            if (!ReadFileBytes(args.skinPath, a.skinPng, kMaxSkinPngBytes) && log) {
                log->push_back("could not read skin " + args.skinPath);
            }
        }
        if (!args.capePath.empty()) {
            if (!ReadFileBytes(args.capePath, a.capePng, kMaxCapePngBytes) && log) {
                log->push_back("could not read cape " + args.capePath);
            }
        }
        if (!args.stickFigurePath.empty()) {
            std::vector<uint8_t> bytes;
            if (ReadFileBytes(args.stickFigurePath, bytes, kMaxStickFigureFileBytes)) {
                const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                std::string why;
                if (auto file = StickFigureFile::FromText(text, &why)) {
                    a.hasPaint = file->hasPaint;
                    a.paint = file->paint;
                    a.drawing = std::move(file->drawing);
                    if (!why.empty() && log) log->push_back("stick figure " + args.stickFigurePath + ": " + why);
                } else if (log) {
                    log->push_back("stick figure " + args.stickFigurePath + " is " + why);
                }
            } else if (log) {
                log->push_back("could not read stick figure " + args.stickFigurePath);
            }
        }
        a.Sanitize(log);
        return a;
    }

} // namespace Game
