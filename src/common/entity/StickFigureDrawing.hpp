// File: src/common/entity/StickFigureDrawing.hpp
//
// A drawn figure: a picture a player draws in the launcher's drawing editor
// with a round brush, and that every client shows INSTEAD of the stick
// figure — upright at the player, turning with the body, each stroke drawn
// the way the stick figure draws its own limbs (docs/player-appearance.md).
//
// A drawing is a list of strokes, painted in order (a later stroke lies over
// an earlier one). A stroke is one palette colour, one brush radius and a
// polyline of one or more points (one point is a dot). It is round: the
// brush's circle swept along the polyline, so its ends and its joints are
// round.
//
// The canvas. 0.6 × 3.0 blocks:
//
//   width   0.6 blocks, MC's player hitbox (EntityType.PLAYER 0.6 × 1.8) —
//           a drawing is never wider than the body it stands for;
//   height  3.0 blocks above the feet, nothing below them — room for a tall
//           hat or raised arms over the 1.8-block player.
//
// Points sit on a grid of kGridSize = 0.5 mm (1/2000 block): x from 0 to
// kGridWidth (1200), y from 0 to kGridHeight (6000). x = 0 is the canvas's
// left edge AS SEEN FROM THE FRONT — the player's right — and y counts up
// from the feet. In figure space (blocks, feet at the origin, +X the
// player's right, +Y up, +Z the way the body faces) point (x, y) is
//
//   X = 0.3 − x·kGridSize,   Y = y·kGridSize,   Z = 0.
//
// The editor keeps a stroke's whole width inside the canvas; the grid only
// bounds the centre line.
//
// Radius: whole millimetres (1/1000 block), kMinRadius .. kMaxRadius. The
// stick figure's own limbs are kLineRadius (PlayerRenderer's 1.8 cm strip
// half-width) and its head outline kRingRadius (StickFigureGeometry's ring
// half-width) — the editor's default brush is the limb.
//
// Colours: the preset stick-figure colours only (Game::kPlayerColorTable),
// the painter's rule.
//
// Encoding (Encode / Decode; the stick-figure file carries it as hex, the
// PlayerAppearance packets as raw bytes):
//
//   byte  version (4)
//   strokes, until the bytes end:
//     byte    colour       (PlayerColorId)
//     byte    radius       (mm, kMinRadius .. kMaxRadius)
//     VarInt  points − 1   (LEB128)
//     u16le   x, u16le y   the first point
//     (points − 1) × { zigzag VarInt dx, zigzag VarInt dy }   from the last
//
// Version 1 is the earlier pixel canvas (32 × 160 pixels over the same
// 0.6 × 3.0 blocks, run-length encoded — the appearance-v3 drawing). Decode
// still reads it and converts it into strokes: each row's (or column's)
// run of one colour becomes a stroke through the pixel centres, radius
// 10 mm (just over half a pixel), so an old drawing survives as the same
// picture. The version number follows the appearance version that brought
// it (4); 2 and 3 were never drawings.
//
// Caps (hard: Decode refuses, Validate fails, the server's Sanitize drops a
// drawing over them, and the editor will not make one): kMaxStrokes
// strokes, kMaxPoints points in all, kMaxEncodedBytes encoded. They bound
// the in-game geometry (a ribbon and a round joint per point) and the
// packet; an ordinary drawing is a few dozen strokes and a few hundred
// points. The editor simplifies what the mouse draws (Simplify) so a
// stroke costs its shape, not the mouse's sample rate.
//
// Nothing here depends on the renderer or the network: the launcher and the
// server compile it too.
#pragma once

#include "common/entity/PlayerColors.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {

    struct StickFigureDrawing {
        // ── The canvas ──
        static constexpr float kWidthBlocks = 0.6f;                       // MC's player hitbox width
        static constexpr float kHeightBlocks = 3.0f;
        // The player's own height (MC EntityDimensions 1.8) — the editor's guide line.
        static constexpr float kPlayerHeightBlocks = 1.8f;
        static constexpr int   kGridPerBlock = 2000;                      // 0.5 mm
        static constexpr float kGridSize = 1.0f / static_cast<float>(kGridPerBlock);
        static constexpr int   kGridWidth = 1200;                         // 0.6 blocks
        static constexpr int   kGridHeight = 6000;                        // 3.0 blocks

        // ── The brush ──
        static constexpr int   kRadiusPerBlock = 1000;                    // 1 mm
        static constexpr int   kMinRadius = 4;
        static constexpr int   kMaxRadius = 150;
        static constexpr int   kLineRadius = 18;                          // the stick figure's limbs
        static constexpr int   kRingRadius = 25;                          // its head outline and smile

        // ── Caps ──
        static constexpr int     kMaxStrokes = 2048;
        static constexpr int     kMaxPoints = 4096;
        // Every stroke's header is at most 8 bytes (colour, radius, a 2-byte
        // count, the first point) and every further point at most 4 (two
        // 2-byte deltas: |dx| ≤ 1200, |dy| ≤ 6000).
        static constexpr size_t  kMaxEncodedBytes =
            1 + 4 * static_cast<size_t>(kMaxStrokes) + 4 * static_cast<size_t>(kMaxPoints);   // 24,577
        static constexpr uint8_t kEncodingVersion = 4;
        static constexpr uint8_t kPixelEncodingVersion = 1;               // the old canvas, read only

        struct Point {
            uint16_t x = 0;   // grid units from the left as seen from the front
            uint16_t y = 0;   // grid units up from the feet
            bool operator==(const Point& o) const { return x == o.x && y == o.y; }
            bool operator!=(const Point& o) const { return !(*this == o); }
        };
        struct Stroke {
            PlayerColorId      color = PlayerColorId::Default;
            uint8_t            radius = kLineRadius;   // mm
            std::vector<Point> points;                 // at least one
            bool operator==(const Stroke& o) const {
                return color == o.color && radius == o.radius && points == o.points;
            }
            bool operator!=(const Stroke& o) const { return !(*this == o); }
        };

        std::vector<Stroke> strokes;

        // Figure space (blocks) of a grid coordinate, and a radius in blocks.
        static constexpr float FigureX(float gx) { return kWidthBlocks * 0.5f - gx * kGridSize; }
        static constexpr float FigureY(float gy) { return gy * kGridSize; }
        static constexpr float RadiusBlocks(int radius) {
            return static_cast<float>(radius) / static_cast<float>(kRadiusPerBlock);
        }
        // The column mirrored across the canvas's centre line.
        static constexpr int MirrorX(int gx) { return kGridWidth - gx; }
        // The nearest grid point to (gx, gy), clamped into the canvas.
        static Point ToPoint(float gx, float gy);

        bool Empty() const { return strokes.empty(); }
        void Clear() { strokes.clear(); }

        int StrokeCount() const { return static_cast<int>(strokes.size()); }
        int PointCount() const;
        // The highest ink above the feet in blocks (a point's height plus its
        // stroke's radius; 0 when empty).
        float TopBlocks() const;
        // The colour covering the most canvas (roughly: each stroke's length
        // plus its caps, times its width), or `fallback` when empty.
        PlayerColorId DominantColor(PlayerColorId fallback) const;

        // Bytes Encode would produce (0 for an empty drawing).
        size_t EncodedSize() const;
        // How full the drawing is against its caps, 0..1+ (the largest of
        // strokes, points and bytes over their caps) — the editor's meter.
        float CapUse() const;

        // Every colour in the palette, every radius in range, every stroke
        // non-empty, every point in the canvas, within the caps.
        bool Validate(std::string* why = nullptr) const;

        // Empty for an empty drawing.
        std::vector<uint8_t> Encode() const;
        // Nullopt for anything malformed or over a cap (`why` says what).
        // Reads version 4 (strokes) and version 1 (the old pixel canvas,
        // converted to strokes).
        static std::optional<StickFigureDrawing> Decode(const uint8_t* data, size_t size,
                                                        std::string* why = nullptr);
        static std::optional<StickFigureDrawing> Decode(const std::vector<uint8_t>& bytes,
                                                        std::string* why = nullptr) {
            return Decode(bytes.data(), bytes.size(), why);
        }

        // The encoding as lower-case hex (the stick-figure file, launcher.json).
        std::string ToHex() const;
        static std::optional<StickFigureDrawing> FromHex(const std::string& hex, std::string* why = nullptr);

        // Ramer–Douglas–Peucker: drops the points that stray less than
        // `tolerance` grid units from the line their neighbours make. The
        // first and the last point always stay (a closed loop stays closed).
        static void Simplify(std::vector<Point>& points, float tolerance);

        bool operator==(const StickFigureDrawing& o) const { return strokes == o.strokes; }
        bool operator!=(const StickFigureDrawing& o) const { return !(*this == o); }
    };

} // namespace Game
