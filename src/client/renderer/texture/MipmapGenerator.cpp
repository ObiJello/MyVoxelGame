// File: src/client/renderer/texture/MipmapGenerator.cpp
#include "MipmapGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>

namespace Render::Mipmap {

    namespace {

        // ── ARGB.java: the sRGB <-> linear tables ───────────────────────────
        //
        // MC keeps colour in 8-bit sRGB but averages in linear light at 10-bit
        // precision (LINEAR_CHANNEL_DEPTH = 1024). Both tables are built from
        // the piecewise sRGB transfer function, exactly as ARGB.java does.

        constexpr int kLinearDepth = 1024;

        float ComputeSrgbToLinear(float x) {
            return x >= 0.04045f ? std::pow((x + 0.055f) / 1.055f, 2.4f)
                                 : x / 12.92f;
        }

        float ComputeLinearToSrgb(float x) {
            // MC spells the exponent 0.4166666666666667, i.e. 1/2.4.
            return x >= 0.0031308f
                       ? 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f
                       : 12.92f * x;
        }

        struct GammaTables {
            std::array<uint16_t, 256> srgbToLinear{};   // 0..1023
            std::array<uint8_t, kLinearDepth> linearToSrgb{};  // 0..255

            GammaTables() {
                for (int i = 0; i < 256; ++i) {
                    const float channel = static_cast<float>(i) / 255.0f;
                    srgbToLinear[i] = static_cast<uint16_t>(
                        std::lround(ComputeSrgbToLinear(channel) * 1023.0f));
                }
                for (int i = 0; i < kLinearDepth; ++i) {
                    const float channel = static_cast<float>(i) / 1023.0f;
                    linearToSrgb[i] = static_cast<uint8_t>(
                        std::lround(ComputeLinearToSrgb(channel) * 255.0f));
                }
            }
        };

        const GammaTables& Gamma() {
            static const GammaTables tables;
            return tables;
        }

        // ARGB.srgbToLinearChannel
        float SrgbToLinearChannel(int srgb) {
            return static_cast<float>(Gamma().srgbToLinear[srgb & 0xFF]) / 1023.0f;
        }

        // ARGB.linearToSrgbChannel. MC indexes with Mth.floor(linear * 1023);
        // the clamp guards the table rather than trusting the caller's float.
        uint8_t LinearToSrgbChannel(float linear) {
            int idx = static_cast<int>(std::floor(linear * 1023.0f));
            idx = std::clamp(idx, 0, kLinearDepth - 1);
            return Gamma().linearToSrgb[static_cast<size_t>(idx)];
        }

        // ARGB.linearChannelMean — the mean of four channels in linear light.
        uint8_t LinearChannelMean(int c1, int c2, int c3, int c4) {
            const auto& t = Gamma().srgbToLinear;
            const int linear = (t[c1 & 0xFF] + t[c2 & 0xFF] + t[c3 & 0xFF] + t[c4 & 0xFF]) / 4;
            return Gamma().linearToSrgb[static_cast<size_t>(
                std::clamp(linear, 0, kLinearDepth - 1))];
        }

        // ── Pixel access over RGBA8 ─────────────────────────────────────────

        struct Rgba { uint8_t r, g, b, a; };

        Rgba Get(const Image& img, int x, int y) {
            const size_t i = (static_cast<size_t>(y) * img.width + x) * 4u;
            return { img.pixels[i], img.pixels[i + 1], img.pixels[i + 2], img.pixels[i + 3] };
        }

        void Set(Image& img, int x, int y, Rgba c) {
            const size_t i = (static_cast<size_t>(y) * img.width + x) * 4u;
            img.pixels[i]     = c.r;
            img.pixels[i + 1] = c.g;
            img.pixels[i + 2] = c.b;
            img.pixels[i + 3] = c.a;
        }

        // ── ARGB.meanLinear ─────────────────────────────────────────────────
        //
        // Note the asymmetry, which is MC's and not a transcription slip:
        // ALPHA is a plain arithmetic mean while RGB goes through the linear
        // tables. Alpha is a coverage weight, not a light value, so gamma does
        // not apply to it.
        Rgba MeanLinear(Rgba c1, Rgba c2, Rgba c3, Rgba c4) {
            return Rgba{
                LinearChannelMean(c1.r, c2.r, c3.r, c4.r),
                LinearChannelMean(c1.g, c2.g, c3.g, c4.g),
                LinearChannelMean(c1.b, c2.b, c3.b, c4.b),
                static_cast<uint8_t>((int(c1.a) + int(c2.a) + int(c3.a) + int(c4.a)) / 4)
            };
        }

        // ── MipmapGenerator.darkenedAlphaBlend ──────────────────────────────
        //
        // Fully transparent texels are skipped entirely, but the totals are
        // still divided by four rather than by the number that contributed —
        // that deliberate under-weighting is what darkens the result. Here
        // alpha DOES go through the gamma tables (unlike meanLinear above);
        // both quirks are vanilla's.
        Rgba DarkenedAlphaBlend(Rgba c1, Rgba c2, Rgba c3, Rgba c4) {
            float aTotal = 0.0f, rTotal = 0.0f, gTotal = 0.0f, bTotal = 0.0f;
            for (const Rgba& c : { c1, c2, c3, c4 }) {
                if (c.a == 0) continue;
                aTotal += SrgbToLinearChannel(c.a);
                rTotal += SrgbToLinearChannel(c.r);
                gTotal += SrgbToLinearChannel(c.g);
                bTotal += SrgbToLinearChannel(c.b);
            }
            aTotal /= 4.0f;
            rTotal /= 4.0f;
            gTotal /= 4.0f;
            bTotal /= 4.0f;
            return Rgba{ LinearToSrgbChannel(rTotal), LinearToSrgbChannel(gTotal),
                         LinearToSrgbChannel(bTotal), LinearToSrgbChannel(aTotal) };
        }

        // ── TextureUtil.solidify ────────────────────────────────────────────
        //
        // Multi-source BFS from every opaque texel, flooding the nearest opaque
        // colour outward. Alpha is left at 0, so the sprite still cuts out
        // identically — only the colour that mipmapping will average changes.
        void Solidify(Image& img) {
            const int w = img.width, h = img.height;
            const size_t n = static_cast<size_t>(w) * h;
            std::vector<Rgba> nearest(n, Rgba{ 0, 0, 0, 0 });
            std::vector<int>  dist(n, std::numeric_limits<int>::max());
            std::deque<int>   queue;

            auto pack = [w](int x, int y) { return x + y * w; };

            for (int x = 0; x < w; ++x) {
                for (int y = 0; y < h; ++y) {
                    const Rgba c = Get(img, x, y);
                    if (c.a != 0) {
                        const int p = pack(x, y);
                        dist[static_cast<size_t>(p)] = 0;
                        nearest[static_cast<size_t>(p)] = c;
                        queue.push_back(p);
                    }
                }
            }

            // All-transparent sprite: nothing to flood from.
            if (queue.empty()) return;

            static constexpr int kDirs[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
            while (!queue.empty()) {
                const int p = queue.front();
                queue.pop_front();
                const int x = p % w;
                const int y = p / w;
                for (const auto& d : kDirs) {
                    const int nx = x + d[0];
                    const int ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t np = static_cast<size_t>(pack(nx, ny));
                    if (dist[np] > dist[static_cast<size_t>(p)] + 1) {
                        dist[np] = dist[static_cast<size_t>(p)] + 1;
                        nearest[np] = nearest[static_cast<size_t>(p)];
                        queue.push_back(static_cast<int>(np));
                    }
                }
            }

            for (int x = 0; x < w; ++x) {
                for (int y = 0; y < h; ++y) {
                    if (Get(img, x, y).a == 0) {
                        Rgba c = nearest[static_cast<size_t>(pack(x, y))];
                        c.a = 0;                       // ARGB.color(0, nearestColor)
                        Set(img, x, y, c);
                    }
                }
            }
        }

        // ── TextureUtil.fillEmptyAreasWithDarkColor ─────────────────────────
        //
        // Paints every transparent texel with 3/4 of the sprite's darkest
        // opaque colour (alpha still 0). Paired with DarkenedAlphaBlend this is
        // how vanilla keeps leaf mips reading as dense shadow instead of
        // fringing bright.
        void FillEmptyAreasWithDarkColor(Image& img) {
            const int w = img.width, h = img.height;
            Rgba darkest{ 255, 255, 255, 255 };
            int minBrightness = std::numeric_limits<int>::max();
            bool found = false;

            for (int x = 0; x < w; ++x) {
                for (int y = 0; y < h; ++y) {
                    const Rgba c = Get(img, x, y);
                    if (c.a == 0) continue;
                    const int brightness = int(c.r) + int(c.g) + int(c.b);
                    if (brightness < minBrightness) {
                        minBrightness = brightness;
                        darkest = c;
                        found = true;
                    }
                }
            }
            if (!found) return;   // fully transparent sprite

            const Rgba darkened{ static_cast<uint8_t>(3 * int(darkest.r) / 4),
                                 static_cast<uint8_t>(3 * int(darkest.g) / 4),
                                 static_cast<uint8_t>(3 * int(darkest.b) / 4),
                                 0 };

            for (int x = 0; x < w; ++x) {
                for (int y = 0; y < h; ++y) {
                    if (Get(img, x, y).a == 0) Set(img, x, y, darkened);
                }
            }
        }

        // ── MipmapGenerator.alphaTestCoverage ───────────────────────────────
        //
        // Fraction of the sprite that survives an alpha test at `alphaRef`,
        // estimated by bilinearly supersampling each 2x2 texel neighbourhood
        // 4x4 times. Sampling rather than counting texels is what lets the
        // rescale below converge on sub-texel coverage.
        float AlphaTestCoverage(const Image& img, float alphaRef, float alphaScale) {
            const int w = img.width, h = img.height;
            if (w < 2 || h < 2) return 0.0f;

            float coverage = 0.0f;
            for (int y = 0; y < h - 1; ++y) {
                for (int x = 0; x < w - 1; ++x) {
                    const float a00 = std::clamp(Get(img, x,     y    ).a / 255.0f * alphaScale, 0.0f, 1.0f);
                    const float a10 = std::clamp(Get(img, x + 1, y    ).a / 255.0f * alphaScale, 0.0f, 1.0f);
                    const float a01 = std::clamp(Get(img, x,     y + 1).a / 255.0f * alphaScale, 0.0f, 1.0f);
                    const float a11 = std::clamp(Get(img, x + 1, y + 1).a / 255.0f * alphaScale, 0.0f, 1.0f);

                    float texelCoverage = 0.0f;
                    for (int sy = 0; sy < 4; ++sy) {
                        const float fy = (static_cast<float>(sy) + 0.5f) / 4.0f;
                        for (int sx = 0; sx < 4; ++sx) {
                            const float fx = (static_cast<float>(sx) + 0.5f) / 4.0f;
                            const float alpha = a00 * (1.0f - fx) * (1.0f - fy)
                                              + a10 * fx * (1.0f - fy)
                                              + a01 * (1.0f - fx) * fy
                                              + a11 * fx * fy;
                            if (alpha > alphaRef) texelCoverage += 1.0f;
                        }
                    }
                    coverage += texelCoverage / 16.0f;
                }
            }
            return coverage / static_cast<float>((w - 1) * (h - 1));
        }

        // ── MipmapGenerator.scaleAlphaToCoverage ────────────────────────────
        //
        // Bisects an alpha multiplier over [0,4] in five steps so this level's
        // alpha-test coverage lands on level 0's. Without it every halving
        // erodes a cutout sprite; with it a distant leaf canopy keeps the same
        // density as a near one.
        void ScaleAlphaToCoverage(Image& img, float desiredCoverage,
                                  float alphaRef, float alphaCutoffBias) {
            float minAlphaScale = 0.0f;
            float maxAlphaScale = 4.0f;
            float alphaScale = 1.0f;
            float bestAlphaScale = 1.0f;
            float bestError = std::numeric_limits<float>::max();

            for (int i = 0; i < 5; ++i) {
                const float currentCoverage = AlphaTestCoverage(img, alphaRef, alphaScale);
                const float error = std::abs(currentCoverage - desiredCoverage);
                if (error < bestError) {
                    bestError = error;
                    bestAlphaScale = alphaScale;
                }

                if (currentCoverage < desiredCoverage) {
                    minAlphaScale = alphaScale;
                } else if (currentCoverage > desiredCoverage) {
                    maxAlphaScale = alphaScale;
                } else {
                    break;
                }
                alphaScale = (minAlphaScale + maxAlphaScale) * 0.5f;
            }

            for (int y = 0; y < img.height; ++y) {
                for (int x = 0; x < img.width; ++x) {
                    Rgba c = Get(img, x, y);
                    float alpha = c.a / 255.0f;
                    // The bare 0.025 is vanilla's, on top of the per-texture bias.
                    alpha = alpha * bestAlphaScale + alphaCutoffBias + 0.025f;
                    alpha = std::clamp(alpha, 0.0f, 1.0f);
                    c.a = static_cast<uint8_t>(std::lround(alpha * 255.0f));
                    Set(img, x, y, c);
                }
            }
        }

        bool HasTransparentPixel(const Image& img) {
            for (size_t i = 3; i < img.pixels.size(); i += 4) {
                if (img.pixels[i] == 0) return true;
            }
            return false;
        }

    } // namespace

    Strategy ParseStrategy(const std::string& name) {
        if (name == "mean")          return Strategy::Mean;
        if (name == "cutout")        return Strategy::Cutout;
        if (name == "strict_cutout") return Strategy::StrictCutout;
        if (name == "dark_cutout")   return Strategy::DarkCutout;
        return Strategy::Auto;
    }

    std::vector<Image> GenerateMipLevels(Image level0,
                                         int maxLevel,
                                         Strategy strategy,
                                         float alphaCutoffBias,
                                         bool isItemTexture) {
        // MipmapStrategy.AUTO resolves by inspection: anything with a fully
        // transparent texel is a cutout sprite, everything else is a mean one.
        if (strategy == Strategy::Auto) {
            strategy = HasTransparentPixel(level0) ? Strategy::Cutout : Strategy::Mean;
        }

        // Level-0 rewrite. MC skips it for item/ sprites, which are never
        // minified enough for the fringe to show and would lose their crisp
        // outline to the flood fill.
        if (!isItemTexture) {
            if (strategy == Strategy::Cutout || strategy == Strategy::StrictCutout) {
                Solidify(level0);
            } else if (strategy == Strategy::DarkCutout) {
                FillEmptyAreasWithDarkColor(level0);
            }
        }

        std::vector<Image> result;
        result.reserve(static_cast<size_t>(maxLevel) + 1);

        const bool isCutoutMip = strategy == Strategy::Cutout
                              || strategy == Strategy::StrictCutout
                              || strategy == Strategy::DarkCutout;
        const float cutoutRef = strategy == Strategy::StrictCutout ? 0.3f : 0.5f;
        const float originalCoverage =
            isCutoutMip ? AlphaTestCoverage(level0, cutoutRef, 1.0f) : 0.0f;

        result.push_back(std::move(level0));

        for (int level = 1; level <= maxLevel; ++level) {
            const Image& prev = result.back();
            // Refuse to halve an odd dimension rather than silently dropping a
            // row; the block atlas never hits this (everything is a multiple
            // of 16) but a stray sprite should shorten the chain, not corrupt it.
            if (prev.width < 2 || prev.height < 2 ||
                (prev.width & 1) || (prev.height & 1)) {
                break;
            }

            Image next;
            next.width  = prev.width >> 1;
            next.height = prev.height >> 1;
            next.pixels.resize(next.ByteSize());

            for (int x = 0; x < next.width; ++x) {
                for (int y = 0; y < next.height; ++y) {
                    const Rgba c1 = Get(prev, x * 2 + 0, y * 2 + 0);
                    const Rgba c2 = Get(prev, x * 2 + 1, y * 2 + 0);
                    const Rgba c3 = Get(prev, x * 2 + 0, y * 2 + 1);
                    const Rgba c4 = Get(prev, x * 2 + 1, y * 2 + 1);
                    Set(next, x, y, strategy == Strategy::DarkCutout
                                        ? DarkenedAlphaBlend(c1, c2, c3, c4)
                                        : MeanLinear(c1, c2, c3, c4));
                }
            }

            if (isCutoutMip) {
                ScaleAlphaToCoverage(next, originalCoverage, cutoutRef, alphaCutoffBias);
            }
            result.push_back(std::move(next));
        }

        return result;
    }

} // namespace Render::Mipmap
