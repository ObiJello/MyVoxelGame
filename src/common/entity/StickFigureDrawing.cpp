// File: src/common/entity/StickFigureDrawing.cpp
#include "StickFigureDrawing.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace Game {

    namespace {

        using Drawing = StickFigureDrawing;
        constexpr int kColorCount = static_cast<int>(PlayerColorId::Count);

        bool Fail(std::string* why, const char* reason) {
            if (why) *why = reason;
            return false;
        }

        int HexValue(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        uint32_t ZigZag(int32_t v) { return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31); }
        int32_t  UnZigZag(uint32_t u) { return static_cast<int32_t>(u >> 1) ^ -static_cast<int32_t>(u & 1u); }

        // Calls put(byte) for every byte Encode writes.
        template <class Put>
        void EncodeTo(const Drawing& d, Put&& put) {
            if (d.strokes.empty()) return;
            const auto varInt = [&](uint32_t v) {
                while (v >= 0x80u) {
                    put(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
                    v >>= 7;
                }
                put(static_cast<uint8_t>(v));
            };
            put(Drawing::kEncodingVersion);
            for (const Drawing::Stroke& s : d.strokes) {
                if (s.points.empty()) continue;
                put(static_cast<uint8_t>(s.color));
                put(s.radius);
                varInt(static_cast<uint32_t>(s.points.size() - 1));
                const Drawing::Point& p0 = s.points.front();
                put(static_cast<uint8_t>(p0.x & 0xFF));
                put(static_cast<uint8_t>(p0.x >> 8));
                put(static_cast<uint8_t>(p0.y & 0xFF));
                put(static_cast<uint8_t>(p0.y >> 8));
                for (size_t i = 1; i < s.points.size(); ++i) {
                    varInt(ZigZag(static_cast<int32_t>(s.points[i].x) - static_cast<int32_t>(s.points[i - 1].x)));
                    varInt(ZigZag(static_cast<int32_t>(s.points[i].y) - static_cast<int32_t>(s.points[i - 1].y)));
                }
            }
        }

        // An LEB128 value of at most three bytes (21 bits — every count and
        // delta the encoding has fits in two).
        bool ReadVarInt(const uint8_t* data, size_t size, size_t& pos, uint32_t& out) {
            out = 0;
            for (int shift = 0; shift < 21; shift += 7) {
                if (pos >= size) return false;
                const uint8_t b = data[pos++];
                out |= static_cast<uint32_t>(b & 0x7Fu) << shift;
                if ((b & 0x80u) == 0) return true;
            }
            return false;
        }

        // ── Version 1: the old pixel canvas ──
        //
        // 32 × 160 pixels over the same 0.6 × 3.0 blocks; pixel (x, y) from the
        // left as seen from the front and from the top row, value 0 for
        // transparent or PlayerColorId + 1; runs of (value, length − 1) in
        // reading order after a 3-byte header (1, 32, 160), capped at 2,048
        // runs (4,099 bytes).
        constexpr int    kPixelW = 32;
        constexpr int    kPixelH = 160;
        constexpr int    kPixelMaxRuns = 2048;
        constexpr size_t kPixelMaxBytes = 3 + 2 * static_cast<size_t>(kPixelMaxRuns);
        // The grid units across one pixel (0.01875 blocks), and the radius
        // that covers a pixel row (just over half of it).
        constexpr float  kPixelGrid = 0.6f / static_cast<float>(kPixelW) * static_cast<float>(Drawing::kGridPerBlock);
        constexpr int    kPixelStrokeRadius = 10;

        std::optional<Drawing> DecodePixels(const uint8_t* data, size_t size, std::string* why) {
            if (size > kPixelMaxBytes) {
                Fail(why, "larger than the old drawing's 4 KB cap");
                return std::nullopt;
            }
            if (size < 3 || data[1] != kPixelW || data[2] != kPixelH) {
                Fail(why, "not a 32x160 canvas");
                return std::nullopt;
            }
            if ((size - 3) % 2 != 0) {
                Fail(why, "truncated run");
                return std::nullopt;
            }
            std::vector<uint8_t> pixels(static_cast<size_t>(kPixelW * kPixelH), 0);
            size_t next = 0;
            for (size_t pos = 3; pos < size; pos += 2) {
                const uint8_t value = data[pos];
                const size_t length = static_cast<size_t>(data[pos + 1]) + 1;
                if (value > kColorCount) {
                    Fail(why, "a pixel colour is not in the palette");
                    return std::nullopt;
                }
                if (next + length > pixels.size()) {
                    Fail(why, "runs leave the canvas");
                    return std::nullopt;
                }
                std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(next), length, value);
                next += length;
            }
            const auto at = [&](int x, int y) { return pixels[static_cast<size_t>(y * kPixelW + x)]; };

            // Each row's runs of one colour, or each column's — whichever
            // takes fewer strokes — as strokes through the pixel centres.
            const auto countRuns = [&](bool rows) {
                int n = 0;
                const int outer = rows ? kPixelH : kPixelW, inner = rows ? kPixelW : kPixelH;
                for (int o = 0; o < outer; ++o) {
                    uint8_t last = 0;
                    for (int i = 0; i < inner; ++i) {
                        const uint8_t v = rows ? at(i, o) : at(o, i);
                        if (v != 0 && v != last) ++n;
                        last = v;
                    }
                }
                return n;
            };
            const bool rows = countRuns(true) <= countRuns(false);
            const auto centre = [](int x, int y) {
                return Drawing::Point{ static_cast<uint16_t>(std::lround((static_cast<float>(x) + 0.5f) * kPixelGrid)),
                                       static_cast<uint16_t>(std::lround((static_cast<float>(kPixelH - y) - 0.5f) *
                                                                         kPixelGrid)) };
            };
            Drawing d;
            int points = 0;
            const int outer = rows ? kPixelH : kPixelW, inner = rows ? kPixelW : kPixelH;
            for (int o = 0; o < outer; ++o) {
                for (int i = 0; i < inner;) {
                    const uint8_t v = rows ? at(i, o) : at(o, i);
                    if (v == 0) { ++i; continue; }
                    int n = 1;
                    while (i + n < inner && (rows ? at(i + n, o) : at(o, i + n)) == v) ++n;
                    Drawing::Stroke s;
                    s.color = static_cast<PlayerColorId>(v - 1);
                    s.radius = kPixelStrokeRadius;
                    s.points.push_back(rows ? centre(i, o) : centre(o, i));
                    if (n > 1) s.points.push_back(rows ? centre(i + n - 1, o) : centre(o, i + n - 1));
                    // A pathological old drawing past the caps keeps what fits.
                    if (d.StrokeCount() >= Drawing::kMaxStrokes ||
                        points + static_cast<int>(s.points.size()) > Drawing::kMaxPoints) {
                        return d;
                    }
                    points += static_cast<int>(s.points.size());
                    d.strokes.push_back(std::move(s));
                    i += n;
                }
            }
            return d;
        }

        float PointLineDistance(const Drawing::Point& p, const Drawing::Point& a, const Drawing::Point& b) {
            const float px = static_cast<float>(p.x) - static_cast<float>(a.x);
            const float py = static_cast<float>(p.y) - static_cast<float>(a.y);
            const float dx = static_cast<float>(b.x) - static_cast<float>(a.x);
            const float dy = static_cast<float>(b.y) - static_cast<float>(a.y);
            const float len2 = dx * dx + dy * dy;
            if (len2 <= 0.0f) return std::sqrt(px * px + py * py);
            const float t = std::clamp((px * dx + py * dy) / len2, 0.0f, 1.0f);
            const float ex = px - dx * t, ey = py - dy * t;
            return std::sqrt(ex * ex + ey * ey);
        }

    } // namespace

    StickFigureDrawing::Point StickFigureDrawing::ToPoint(float gx, float gy) {
        const auto snap = [](float v, int hi) {
            if (!(v > 0.0f)) return uint16_t{0};
            return static_cast<uint16_t>(std::min(static_cast<long>(hi), std::lround(v)));
        };
        return Point{ snap(gx, kGridWidth), snap(gy, kGridHeight) };
    }

    int StickFigureDrawing::PointCount() const {
        int n = 0;
        for (const Stroke& s : strokes) n += static_cast<int>(s.points.size());
        return n;
    }

    float StickFigureDrawing::TopBlocks() const {
        float top = 0.0f;
        for (const Stroke& s : strokes) {
            for (const Point& p : s.points) {
                top = std::max(top, FigureY(static_cast<float>(p.y)) + RadiusBlocks(s.radius));
            }
        }
        return top;
    }

    PlayerColorId StickFigureDrawing::DominantColor(PlayerColorId fallback) const {
        float ink[kColorCount] = {};
        bool any = false;
        for (const Stroke& s : strokes) {
            if (static_cast<int>(s.color) >= kColorCount || s.points.empty()) continue;
            float length = 2.0f * static_cast<float>(s.radius) * static_cast<float>(kGridPerBlock) /
                           static_cast<float>(kRadiusPerBlock);
            for (size_t i = 1; i < s.points.size(); ++i) {
                const float dx = static_cast<float>(s.points[i].x) - static_cast<float>(s.points[i - 1].x);
                const float dy = static_cast<float>(s.points[i].y) - static_cast<float>(s.points[i - 1].y);
                length += std::sqrt(dx * dx + dy * dy);
            }
            ink[static_cast<int>(s.color)] += length * static_cast<float>(s.radius);
            any = true;
        }
        if (!any) return fallback;
        return static_cast<PlayerColorId>(std::max_element(std::begin(ink), std::end(ink)) - std::begin(ink));
    }

    size_t StickFigureDrawing::EncodedSize() const {
        size_t n = 0;
        EncodeTo(*this, [&](uint8_t) { ++n; });
        return n;
    }

    float StickFigureDrawing::CapUse() const {
        const float s = static_cast<float>(StrokeCount()) / static_cast<float>(kMaxStrokes);
        const float p = static_cast<float>(PointCount()) / static_cast<float>(kMaxPoints);
        const float b = static_cast<float>(EncodedSize()) / static_cast<float>(kMaxEncodedBytes);
        return std::max(s, std::max(p, b));
    }

    bool StickFigureDrawing::Validate(std::string* why) const {
        if (strokes.size() > static_cast<size_t>(kMaxStrokes)) return Fail(why, "more strokes than the 2048 cap");
        int points = 0;
        for (const Stroke& s : strokes) {
            if (static_cast<int>(s.color) >= kColorCount) return Fail(why, "a stroke colour is not in the palette");
            if (s.radius < kMinRadius || s.radius > kMaxRadius) return Fail(why, "a brush size is out of range");
            if (s.points.empty()) return Fail(why, "a stroke has no points");
            for (const Point& p : s.points) {
                if (p.x > kGridWidth || p.y > kGridHeight) return Fail(why, "a stroke leaves the canvas");
            }
            points += static_cast<int>(s.points.size());
            if (points > kMaxPoints) return Fail(why, "more points than the 4096 cap");
        }
        if (EncodedSize() > kMaxEncodedBytes) return Fail(why, "larger than the 24 KB cap");
        return true;
    }

    std::vector<uint8_t> StickFigureDrawing::Encode() const {
        std::vector<uint8_t> out;
        if (Empty()) return out;
        out.reserve(EncodedSize());
        EncodeTo(*this, [&](uint8_t b) { out.push_back(b); });
        return out;
    }

    std::optional<StickFigureDrawing> StickFigureDrawing::Decode(const uint8_t* data, size_t size,
                                                                 std::string* why) {
        if (!data || size < 1) {
            Fail(why, "truncated");
            return std::nullopt;
        }
        if (data[0] == kPixelEncodingVersion) return DecodePixels(data, size, why);
        if (data[0] != kEncodingVersion) {
            Fail(why, "unknown drawing version");
            return std::nullopt;
        }
        if (size > kMaxEncodedBytes) {
            Fail(why, "larger than the 24 KB cap");
            return std::nullopt;
        }
        StickFigureDrawing d;
        int points = 0;
        size_t pos = 1;
        while (pos < size) {
            if (size - pos < 2) {
                Fail(why, "truncated stroke");
                return std::nullopt;
            }
            Stroke s;
            const uint8_t color = data[pos++];
            s.radius = data[pos++];
            if (color >= kColorCount) {
                Fail(why, "a stroke colour is not in the palette");
                return std::nullopt;
            }
            if (s.radius < kMinRadius || s.radius > kMaxRadius) {
                Fail(why, "a brush size is out of range");
                return std::nullopt;
            }
            s.color = static_cast<PlayerColorId>(color);
            uint32_t more = 0;
            if (!ReadVarInt(data, size, pos, more)) {
                Fail(why, "truncated stroke");
                return std::nullopt;
            }
            if (d.StrokeCount() + 1 > kMaxStrokes) {
                Fail(why, "more strokes than the 2048 cap");
                return std::nullopt;
            }
            if (more >= static_cast<uint32_t>(kMaxPoints) ||
                points + static_cast<int>(more) + 1 > kMaxPoints) {
                Fail(why, "more points than the 4096 cap");
                return std::nullopt;
            }
            if (size - pos < 4) {
                Fail(why, "truncated stroke");
                return std::nullopt;
            }
            int32_t x = static_cast<int32_t>(data[pos] | (data[pos + 1] << 8));
            int32_t y = static_cast<int32_t>(data[pos + 2] | (data[pos + 3] << 8));
            pos += 4;
            s.points.reserve(static_cast<size_t>(more) + 1);
            for (uint32_t i = 0;; ++i) {
                if (x < 0 || y < 0 || x > kGridWidth || y > kGridHeight) {
                    Fail(why, "a stroke leaves the canvas");
                    return std::nullopt;
                }
                s.points.push_back(Point{ static_cast<uint16_t>(x), static_cast<uint16_t>(y) });
                if (i == more) break;
                uint32_t dx = 0, dy = 0;
                if (!ReadVarInt(data, size, pos, dx) || !ReadVarInt(data, size, pos, dy)) {
                    Fail(why, "truncated stroke");
                    return std::nullopt;
                }
                x += UnZigZag(dx);
                y += UnZigZag(dy);
            }
            points += static_cast<int>(s.points.size());
            d.strokes.push_back(std::move(s));
        }
        return d;
    }

    std::string StickFigureDrawing::ToHex() const {
        static constexpr char kHex[] = "0123456789abcdef";
        const std::vector<uint8_t> bytes = Encode();
        std::string out;
        out.reserve(bytes.size() * 2);
        for (const uint8_t b : bytes) {
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0x0F]);
        }
        return out;
    }

    std::optional<StickFigureDrawing> StickFigureDrawing::FromHex(const std::string& hex, std::string* why) {
        std::vector<uint8_t> bytes;
        bytes.reserve(hex.size() / 2);
        int high = -1;
        for (const char c : hex) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            const int v = HexValue(c);
            if (v < 0) {
                Fail(why, "not hex");
                return std::nullopt;
            }
            if (high < 0) {
                high = v;
            } else {
                bytes.push_back(static_cast<uint8_t>((high << 4) | v));
                high = -1;
            }
            if (bytes.size() > kMaxEncodedBytes) {
                Fail(why, "larger than the 24 KB cap");
                return std::nullopt;
            }
        }
        if (high >= 0) {
            Fail(why, "odd number of hex digits");
            return std::nullopt;
        }
        return Decode(bytes, why);
    }

    void StickFigureDrawing::Simplify(std::vector<Point>& points, float tolerance) {
        if (points.size() < 3) return;
        std::vector<uint8_t> keep(points.size(), 0);
        keep.front() = keep.back() = 1;
        std::vector<std::pair<size_t, size_t>> stack{ { 0, points.size() - 1 } };
        while (!stack.empty()) {
            const auto [first, last] = stack.back();
            stack.pop_back();
            if (last <= first + 1) continue;
            float worst = -1.0f;
            size_t at = first;
            for (size_t i = first + 1; i < last; ++i) {
                const float d = PointLineDistance(points[i], points[first], points[last]);
                if (d > worst) {
                    worst = d;
                    at = i;
                }
            }
            if (worst > tolerance) {
                keep[at] = 1;
                stack.push_back({ first, at });
                stack.push_back({ at, last });
            }
        }
        size_t out = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            if (keep[i]) points[out++] = points[i];
        }
        points.resize(out);
    }

} // namespace Game
