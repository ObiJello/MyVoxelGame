// File: src/common/world/biome/Biomes.cpp
#include "Biomes.hpp"
#include "../../core/Log.hpp"

#include "synth/PerlinSimplexNoise.h"   // terrain library: Biome.BIOME_INFO_NOISE

#include "stb_image.h"   // ext/stb_image is on the target's include path

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Game {

    namespace {

        // ── The generated table ─────────────────────────────────────────────
        constexpr BiomeInfo kBiomes[] = {
        #define BIOME_DEF(slug, temp, down, water, hasG, g, hasF, f, hasD, d, mod) \
            BiomeInfo{ slug, temp, down, water, hasG, g, hasF, f, hasD, d, mod },
        #include "GeneratedBiomeTable.inc"
        #undef BIOME_DEF
        };
        constexpr size_t kBiomeCount = sizeof(kBiomes) / sizeof(kBiomes[0]);

        // See kFallbackBiomeId. Every accessor that has no biome data returns a
        // bare 0 rather than calling in here, so this is the one place the
        // assumption can be checked — and it must be checked, because the slot
        // is decided by tools/gen_biomes.py's ordering. Alphabetically that
        // slot is badlands, whose hard-coded tan grass override would then tint
        // every unloaded or missing sample olive.
        static_assert(kBiomes[kFallbackBiomeId].name == std::string_view("plains"),
                      "Biome id 0 must be plains: it is the fallback every "
                      "accessor returns for missing data, matching MC's "
                      "ClientLevel.getUncachedNoiseBiome. Re-run "
                      "tools/gen_biomes.py, which pins it.");

        // MC's documented fallbacks when a colormap has not been loaded.
        // Signed ints in vanilla; the ARGB alpha is dropped here since every
        // caller works in RGB.
        constexpr uint32_t kGrassDefault   = 0xFFFFFF & static_cast<uint32_t>(-65281);    // GrassColor.get
        constexpr uint32_t kFoliageDefault = 0xFFFFFF & static_cast<uint32_t>(-12012264); // FoliageColor.FOLIAGE_DEFAULT
        constexpr uint32_t kDryDefault     = 0xFFFFFF & static_cast<uint32_t>(-10732494); // DryFoliageColor.FOLIAGE_DRY_DEFAULT

        struct Colormap {
            std::vector<uint32_t> pixels;   // 256*256 RGB, row-major (y*256 + x)
            bool loaded = false;
        };

        Colormap s_grassMap, s_foliageMap, s_dryMap;

        // Per-biome results of the colormap lookup, resolved once at load.
        // MC recomputes these on every query; they are pure functions of the
        // biome, so caching them turns a tint lookup into an array read — which
        // matters because the 5x5 blend asks 25 times per tinted face.
        struct Resolved {
            uint32_t grassBase = kGrassDefault;   // BEFORE the modifier
            uint32_t foliage   = kFoliageDefault;
            uint32_t dry       = kDryDefault;
        };
        std::array<Resolved, kBiomeCount> s_resolved{};

        std::unordered_map<std::string_view, BiomeId>& NameIndex() {
            static std::unordered_map<std::string_view, BiomeId> index = [] {
                std::unordered_map<std::string_view, BiomeId> m;
                m.reserve(kBiomeCount);
                for (size_t i = 0; i < kBiomeCount; ++i) {
                    m.emplace(kBiomes[i].name, static_cast<BiomeId>(i));
                }
                return m;
            }();
            return index;
        }

        BiomeId FallbackId() {
            static const BiomeId id = [] {
                auto it = NameIndex().find("plains");
                return it != NameIndex().end() ? it->second : BiomeId{0};
            }();
            return id;
        }

        bool LoadOne(const std::string& path, Colormap& out) {
            int w = 0, h = 0, channels = 0;
            unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 3);
            if (!data) {
                Log::Warning("Colormap missing: %s - biome tint falls back to vanilla's "
                             "no-colormap constant", path.c_str());
                return false;
            }
            if (w != 256 || h != 256) {
                Log::Warning("Colormap %s is %dx%d, expected 256x256", path.c_str(), w, h);
                stbi_image_free(data);
                return false;
            }
            out.pixels.resize(256 * 256);
            for (int i = 0; i < 256 * 256; ++i) {
                out.pixels[i] = (static_cast<uint32_t>(data[i * 3 + 0]) << 16)
                              | (static_cast<uint32_t>(data[i * 3 + 1]) << 8)
                              |  static_cast<uint32_t>(data[i * 3 + 2]);
            }
            stbi_image_free(data);
            out.loaded = true;
            return true;
        }

        uint32_t Sample(const Colormap& map, float temperature, float downfall, uint32_t fallback) {
            if (!map.loaded) return fallback;
            // MC clamps both to [0,1] before the lookup
            // (Biome.getGrassColorFromTexture uses Mth.clamp). Badlands sits at
            // temperature 2.0, so without this it would index off the map.
            const double t = std::clamp(static_cast<double>(temperature), 0.0, 1.0);
            const double r = std::clamp(static_cast<double>(downfall), 0.0, 1.0);
            return ColorMapLookup(t, r, map.pixels.data(), map.pixels.size(), fallback);
        }

    } // namespace

    uint32_t ColorMapLookup(double temperature, double rainfall,
                            const uint32_t* pixels, size_t pixelCount,
                            uint32_t fallback) {
        // MC ColorMapColorUtil.get, verbatim:
        //   rain *= temp;
        //   x = (int)((1 - temp) * 255);
        //   y = (int)((1 - rain) * 255);
        //   idx = y << 8 | x;
        // The rain *= temp step is easy to miss and shifts the whole lookup.
        rainfall *= temperature;
        const int x = static_cast<int>((1.0 - temperature) * 255.0);
        const int y = static_cast<int>((1.0 - rainfall) * 255.0);
        const size_t index = static_cast<size_t>((y << 8) | x);
        return (!pixels || index >= pixelCount) ? fallback : pixels[index];
    }

    namespace BiomeRegistry {

        BiomeId Count() { return static_cast<BiomeId>(kBiomeCount); }

        const BiomeInfo& Get(BiomeId id) {
            return kBiomes[id < kBiomeCount ? id : FallbackId()];
        }

        BiomeId Fallback() { return FallbackId(); }

        BiomeId FromName(std::string_view name) {
            if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
            auto it = NameIndex().find(name);
            return it != NameIndex().end() ? it->second : FallbackId();
        }

        bool LoadColormaps(const std::string& texturesRoot) {
            const bool g = LoadOne(texturesRoot + "/colormap/grass.png", s_grassMap);
            const bool f = LoadOne(texturesRoot + "/colormap/foliage.png", s_foliageMap);
            const bool d = LoadOne(texturesRoot + "/colormap/dry_foliage.png", s_dryMap);

            // An explicit override in the biome JSON wins outright — MC checks
            // `grassColorOverride().isPresent()` BEFORE ever touching the
            // colormap (Biome.getBaseGrassColor), which is how badlands stays
            // its fixed tan at temperature 2.0.
            for (size_t i = 0; i < kBiomeCount; ++i) {
                const BiomeInfo& b = kBiomes[i];
                Resolved& r = s_resolved[i];
                r.grassBase = b.hasGrassOverride
                                  ? b.grassColor
                                  : Sample(s_grassMap, b.temperature, b.downfall, kGrassDefault);
                r.foliage   = b.hasFoliageOverride
                                  ? b.foliageColor
                                  : Sample(s_foliageMap, b.temperature, b.downfall, kFoliageDefault);
                r.dry       = b.hasDryFoliageOverride
                                  ? b.dryFoliageColor
                                  : Sample(s_dryMap, b.temperature, b.downfall, kDryDefault);
            }

            Log::Info("Biome colours ready - %zu biomes, colormaps grass=%s foliage=%s dry=%s",
                      kBiomeCount, g ? "ok" : "MISSING", f ? "ok" : "MISSING", d ? "ok" : "MISSING");
            // Vanilla reference values, so a wrong table or a failed colormap
            // is visible in the log instead of only on screen:
            //   plains #91BD59  forest #79C05A  jungle #59C93C
            for (const char* probe : {"plains", "forest", "jungle"}) {
                const BiomeId id = FromName(probe);
                Log::Info("  biome[%u] %-8s grass=#%06X foliage=#%06X water=#%06X",
                          static_cast<unsigned>(id), probe,
                          s_resolved[id].grassBase, s_resolved[id].foliage,
                          kBiomes[id].waterColor);
            }
            return g && f && d;
        }

        uint32_t GrassColor(BiomeId id, int worldX, int worldZ) {
            const size_t idx = id < kBiomeCount ? id : FallbackId();
            const uint32_t base = s_resolved[idx].grassBase;

            switch (kBiomes[idx].grassModifier) {
                case GrassColorModifier::None:
                    return base;

                // BiomeSpecialEffects.java:61-63
                //   (baseColor & 16711422) + 2634762 >> 1
                // Note the precedence: the shift applies to the whole sum.
                case GrassColorModifier::DarkForest:
                    return ((base & 0xFEFEFEu) + 0x28340Au) >> 1;

                // BiomeSpecialEffects.java:66-69 — the base colour is DISCARDED;
                // swamp grass is one of two fixed shades chosen by a noise field.
                // Biome.BIOME_INFO_NOISE is PerlinSimplexNoise(seed 2345,
                // octaves [0]), already ported in the terrain library.
                case GrassColorModifier::Swamp: {
                    const double v = minecraft::synth::BiomeInfoNoise::getValue(
                        static_cast<double>(worldX) * 0.0225,
                        static_cast<double>(worldZ) * 0.0225);
                    return v < -0.1 ? 0x4C763Cu : 0x6A7039u;
                }
            }
            return base;
        }

        uint32_t FoliageColor(BiomeId id) {
            return s_resolved[id < kBiomeCount ? id : FallbackId()].foliage;
        }

        uint32_t DryFoliageColor(BiomeId id) {
            return s_resolved[id < kBiomeCount ? id : FallbackId()].dry;
        }

        uint32_t WaterColor(BiomeId id) {
            return Get(id).waterColor;
        }

    } // namespace BiomeRegistry

} // namespace Game
