// File: src/common/entity/PlayerAppearance.hpp
//
// How a player looks: the engine's stick figure (optionally painted cell by
// cell from the stick-figure palette, or replaced by a drawing of round
// strokes the player made — StickFigureDrawing), or a Minecraft player model in a skin
// (classic or slim arms, MC PlayerModelType WIDE / SLIM) with an optional
// cape. Shared by the launcher (which builds it), the game's CLI (which loads
// it from the launcher's files), the network (PlayerAppearancePackets.hpp —
// each client sends its own, the server relays everyone's) and the renderers.
// Data flow: docs/player-appearance.md.
//
// Nothing here depends on the renderer or the network, so the launcher
// compiles it too.
#pragma once

#include "common/entity/PlayerColors.hpp"
#include "common/entity/StickFigureDrawing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    enum class AppearanceMode : uint8_t {
        StickFigure = 0,   // the engine's own figure (PlayerColors / StickFigurePaint)
        Skin        = 1,   // MC PlayerModel in a 64x64 skin
    };

    // MC PlayerModelType: WIDE (Steve's 4-px arms) or SLIM (Alex's 3-px arms).
    enum class SkinModel : uint8_t {
        Classic = 0,
        Slim    = 1,
    };

    const char* AppearanceModeSlug(AppearanceMode mode);       // "stick" / "skin"
    std::optional<AppearanceMode> ParseAppearanceMode(std::string_view slug);
    const char* SkinModelSlug(SkinModel model);                // "classic" / "slim"
    // "classic", "wide", "default", "steve" → Classic; "slim", "alex" → Slim.
    std::optional<SkinModel> ParseSkinModel(std::string_view slug);

    // MC PlayerModelPart's bits (the skin customisation toggles). Every part
    // is shown unless a client says otherwise.
    namespace ModelPartBits {
        inline constexpr uint8_t Cape        = 1u << 0;
        inline constexpr uint8_t Jacket      = 1u << 1;
        inline constexpr uint8_t LeftSleeve  = 1u << 2;
        inline constexpr uint8_t RightSleeve = 1u << 3;
        inline constexpr uint8_t LeftPants   = 1u << 4;
        inline constexpr uint8_t RightPants  = 1u << 5;
        inline constexpr uint8_t Hat         = 1u << 6;
        inline constexpr uint8_t All         = 0x7Fu;
    }

    // ── The painted stick figure ────────────────────────────────────────────
    //
    // The stick figure (client StickFigureGeometry) split into colour cells,
    // each holding a PlayerColorId — the painter's only palette is the preset
    // stick-figure colours. Cell order is part by part, and inside a part:
    //
    //   Torso       8   neck → hip (the seated pelvis line takes the last)
    //   LeftArm     6   shoulder → hand   (left = the PLAYER's left)
    //   RightArm    6   shoulder → hand
    //   LeftLeg     6   hip → foot
    //   RightLeg    6   hip → foot
    //   HeadRing   16   arcs of the head outline: cell 0 starts at the
    //                   player's right (the viewer's left, facing them) and
    //                   the cells run up over the crown, down the player's
    //                   left and back under the chin
    //   Eyes        2   left eye, right eye
    //   Smile       4   the mouth arc, from the player's left to their right
    //   BackOfHead  8   wedges of the back-of-head disc, from the same start
    //                   angle and in the same direction as the outline
    //
    // 62 cells. The text form (StickFigurePaint::ToText) is one header line
    // and one hex digit per cell — the version-1 stick-figure file; the
    // launcher now writes StickFigureFile (version 4), which can carry a
    // drawing as well. The wire carries the raw cell bytes.
    struct StickFigurePaint {
        enum class Part : uint8_t {
            Torso = 0, LeftArm, RightArm, LeftLeg, RightLeg,
            HeadRing, Eyes, Smile, BackOfHead,
            Count
        };
        static constexpr int kPartCount = static_cast<int>(Part::Count);
        static constexpr int kCellsPerPart[kPartCount] = { 8, 6, 6, 6, 6, 16, 2, 4, 8 };

        static constexpr int CellCount(Part p) { return kCellsPerPart[static_cast<int>(p)]; }
        static constexpr int FirstCell(Part p) {
            int first = 0;
            for (int i = 0; i < static_cast<int>(p); ++i) first += kCellsPerPart[i];
            return first;
        }
        static constexpr int kCellCount = 8 + 6 + 6 + 6 + 6 + 16 + 2 + 4 + 8;   // 62

        // The part a cell belongs to, and its index inside it.
        static Part PartOf(int cell);
        static int  IndexInPart(int cell) { return cell - FirstCell(PartOf(cell)); }
        static const char* PartName(Part p);

        std::array<uint8_t, kCellCount> cells{};   // PlayerColorId values

        static StickFigurePaint Uniform(PlayerColorId color);

        PlayerColorId At(int cell) const;
        PlayerColorId At(Part p, int index) const { return At(FirstCell(p) + index); }
        void Set(int cell, PlayerColorId color);
        void Set(Part p, int index, PlayerColorId color) { Set(FirstCell(p) + index, color); }
        void Fill(Part p, PlayerColorId color);

        // Every cell the same colour — the plain stick figure.
        bool IsUniform() const;
        // Out-of-range colour ids become Default.
        void Sanitize();

        std::string ToText() const;
        static std::optional<StickFigurePaint> FromText(std::string_view text);

        bool operator==(const StickFigurePaint& o) const { return cells == o.cells; }
        bool operator!=(const StickFigurePaint& o) const { return !(*this == o); }
    };
    static_assert(StickFigurePaint::FirstCell(StickFigurePaint::Part::Count) ==
                      StickFigurePaint::kCellCount,
                  "kCellCount must be the sum of kCellsPerPart");

    // ── The stick-figure file (--stick-figure) ──────────────────────────────
    //
    // Version 4, what the launcher writes:
    //
    //   obeycraft-stickfigure 4
    //   paint <62 hex digits>        optional: the painted cells
    //   drawing <hex>                optional: StickFigureDrawing::Encode —
    //                                the drawn figure's strokes, worn
    //                                instead of the stick figure
    //
    // Unknown lines are skipped. Older files still load: version 3 (the same
    // lines, its drawing the old pixel canvas — StickFigureDrawing::Decode
    // converts it to strokes), version 2 (paint, plus `sculpt <hex>` — the
    // retired 3D voxels, skipped) and version 1 (the header
    // `obeycraft-stickfigure 1` and the 62 digits, StickFigurePaint::ToText),
    // each a painted or plain figure.
    struct StickFigureFile {
        bool               hasPaint = false;
        StickFigurePaint   paint;
        StickFigureDrawing drawing;

        // Anything worth a file: paint or a drawing.
        bool Any() const { return hasPaint || !drawing.Empty(); }

        std::string ToText() const;
        // Nullopt for a file of no known version; `why` says what. A drawing
        // that fails to decode is dropped alone (the paint still loads) and
        // is reported in `why` too, as is a version-2 file's skipped sculpt.
        static std::optional<StickFigureFile> FromText(std::string_view text, std::string* why = nullptr);
    };
    // The file's size cap: the header, the paint line and the largest drawing
    // in hex (StickFigureDrawing::kMaxEncodedBytes, ~49 KB of hex) — or a
    // version-2 file's sculpt line, which is read and skipped.
    inline constexpr size_t kMaxStickFigureFileBytes = 64 * 1024;
    static_assert(2 * StickFigureDrawing::kMaxEncodedBytes + 256 <= kMaxStickFigureFileBytes,
                  "the largest drawing's hex line must fit the stick-figure file");

    // ── Skin and cape images ────────────────────────────────────────────────

    // An RGBA8 image, rows top to bottom.
    struct SkinImage {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;

        bool Valid() const {
            return width > 0 && height > 0 &&
                   rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        }
        uint8_t* PixelPtr(int x, int y) { return rgba.data() + (static_cast<size_t>(y) * width + x) * 4u; }
        const uint8_t* PixelPtr(int x, int y) const {
            return rgba.data() + (static_cast<size_t>(y) * width + x) * 4u;
        }
    };

    // Decodes a PNG (any channel count) to RGBA8. False on anything that is
    // not a decodable image.
    bool DecodePng(const uint8_t* data, size_t size, SkinImage& out);
    inline bool DecodePng(const std::vector<uint8_t>& png, SkinImage& out) {
        return DecodePng(png.data(), png.size(), out);
    }

    // MC SkinTextureDownloader.processLegacySkin: a 64x32 (pre-1.8) skin is
    // grown to 64x64 with the left arm and leg mirrored from the right; the
    // base layer is forced opaque (setNoAlpha) and a legacy hat that is
    // entirely opaque is cleared (doNotchTransparencyHack). False for any
    // size other than 64x64 / 64x32 (MC discards those).
    bool ProcessLegacySkin(SkinImage& image);

    // Wire and file caps. A 64x64 skin compresses to a few KB; the caps only
    // stop a hostile client making the server relay megabytes.
    inline constexpr size_t kMaxSkinPngBytes = 64 * 1024;
    inline constexpr size_t kMaxCapePngBytes = 64 * 1024;

    // A skin is a PNG of 64x64 or the legacy 64x32. `why` gets the reason on
    // failure.
    bool ValidateSkinPng(const std::vector<uint8_t>& png, std::string* why = nullptr);
    // A cape is a PNG of 64x32 (MC's cape sheet: PlayerCapeModel lays its
    // 10x16x1 box over a 64x32 region — texScale 1 x 0.5 of the 64x64 layer).
    bool ValidateCapePng(const std::vector<uint8_t>& png, std::string* why = nullptr);

    // MC DefaultPlayerSkin's Steve (wide) and Alex (slim), the look of a Skin
    // player who sent no skin of their own.
    const char* DefaultSkinAssetPath(SkinModel model);

    // ── The whole look ──────────────────────────────────────────────────────

    struct PlayerAppearance {
        AppearanceMode mode = AppearanceMode::StickFigure;
        SkinModel      model = SkinModel::Classic;
        uint8_t        modelParts = ModelPartBits::All;
        // Skin mode: the PNG; empty = the model's default (Steve / Alex).
        std::vector<uint8_t> skinPng;
        // The cape PNG (64x32); empty = no cape. Worn in Skin mode only.
        std::vector<uint8_t> capePng;
        // StickFigure mode: the painted figure; without it the figure takes
        // the player's single colour (PlayerColorId, sent at login).
        bool             hasPaint = false;
        StickFigurePaint paint;
        // StickFigure mode: the drawn figure; empty = none. When there is one
        // it REPLACES the stick figure (plain or painted) entirely.
        StickFigureDrawing drawing;

        bool IsSkin() const { return mode == AppearanceMode::Skin; }
        // The drawing is what this player looks like.
        bool IsDrawn() const { return !IsSkin() && !drawing.Empty(); }
        bool HasCape() const { return IsSkin() && !capePng.empty(); }
        bool ShowsPart(uint8_t bit) const { return (modelParts & bit) != 0; }

        // Drops what fails validation — a bad skin falls back to the default
        // skin, a bad cape to none, a drawing that is malformed or over a cap
        // (StickFigureDrawing) to none — and clamps the paint. `log`
        // (optional) gets one line per thing dropped.
        void Sanitize(std::vector<std::string>* log = nullptr);

        bool operator==(const PlayerAppearance& o) const {
            return mode == o.mode && model == o.model && modelParts == o.modelParts &&
                   skinPng == o.skinPng && capePng == o.capePng &&
                   hasPaint == o.hasPaint && (!hasPaint || paint == o.paint) &&
                   drawing == o.drawing;
        }
        bool operator!=(const PlayerAppearance& o) const { return !(*this == o); }
    };

    // ── Launcher → game handoff (the CLI) ───────────────────────────────────
    //
    //   --skin-mode stick|skin      --skin <png>       --skin-model classic|slim
    //   --cape <png>                --stick-figure <txt>
    //
    // The raw strings; LoadAppearanceFromArgs reads the files and validates.
    struct AppearanceArgs {
        std::string mode;
        std::string skinPath;
        std::string model;
        std::string capePath;
        std::string stickFigurePath;

        bool Any() const {
            return !mode.empty() || !skinPath.empty() || !model.empty() ||
                   !capePath.empty() || !stickFigurePath.empty();
        }
    };
    PlayerAppearance LoadAppearanceFromArgs(const AppearanceArgs& args,
                                            std::vector<std::string>* log = nullptr);

    // Whole-file read with a size cap. False if missing, unreadable or larger.
    bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes);

} // namespace Game
