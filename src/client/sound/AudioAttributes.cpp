// File: src/client/sound/AudioAttributes.cpp
#include "client/sound/AudioAttributes.hpp"

#include "client/world/ClientChunkManager.hpp"
#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace Client::AudioAttributes {

    namespace fs = std::filesystem;

    namespace {

        // One layer's audio attributes; unset = "this layer says nothing".
        struct Layer {
            std::optional<BackgroundMusic> music;
            std::optional<AmbientSounds>   ambient;
            std::optional<float>           musicVolume;
        };

        bool                                        g_loaded = false;
        std::vector<Layer>                          g_biomes;       // by BiomeId
        std::array<Layer, Game::kDimensionCount>    g_dimensions;   // by DimensionSlot

        const BackgroundMusic g_noMusic{};
        const AmbientSounds   g_noAmbience{};

        std::string StripNamespace(std::string id) {
            constexpr std::string_view kPrefix = "minecraft:";
            if (id.compare(0, kPrefix.size(), kPrefix) == 0) id.erase(0, kPrefix.size());
            return id;
        }

        // A SoundEvent.CODEC value: a registry id, or MC's direct form
        // {"sound_id": "...", "range": n}.
        std::string ReadSound(const nlohmann::json& j) {
            if (j.is_string()) return StripNamespace(j.get<std::string>());
            if (j.is_object() && j.contains("sound_id") && j["sound_id"].is_string()) {
                return StripNamespace(j["sound_id"].get<std::string>());
            }
            return {};
        }

        std::optional<Music> ReadMusic(const nlohmann::json& j) {
            if (!j.is_object() || !j.contains("sound")) return std::nullopt;
            Music m;
            m.sound = ReadSound(j["sound"]);
            m.minDelay = j.value("min_delay", 0);
            m.maxDelay = j.value("max_delay", 0);
            m.replaceCurrentMusic = j.value("replace_current_music", false);
            if (m.sound.empty()) return std::nullopt;
            return m;
        }

        BackgroundMusic ReadBackgroundMusic(const nlohmann::json& j) {
            BackgroundMusic b;
            if (!j.is_object()) return b;
            if (j.contains("default"))    b.defaultMusic    = ReadMusic(j["default"]);
            if (j.contains("creative"))   b.creativeMusic   = ReadMusic(j["creative"]);
            if (j.contains("underwater")) b.underwaterMusic = ReadMusic(j["underwater"]);
            return b;
        }

        AmbientSounds ReadAmbientSounds(const nlohmann::json& j) {
            AmbientSounds a;
            if (!j.is_object()) return a;
            if (j.contains("loop")) a.loop = ReadSound(j["loop"]);
            if (j.contains("mood") && j["mood"].is_object()) {
                const nlohmann::json& m = j["mood"];
                AmbientMoodSettings mood;
                mood.sound = m.contains("sound") ? ReadSound(m["sound"]) : std::string();
                mood.tickDelay = m.value("tick_delay", 6000);
                mood.blockSearchExtent = m.value("block_search_extent", 8);
                mood.soundPositionOffset = m.value("offset", 2.0);
                if (!mood.sound.empty() && mood.tickDelay > 0) a.mood = mood;
            }
            if (j.contains("additions")) {
                // ExtraCodecs.compactListCodec: one object or a list of them.
                const nlohmann::json& add = j["additions"];
                auto readOne = [&a](const nlohmann::json& e) {
                    if (!e.is_object() || !e.contains("sound")) return;
                    AmbientAdditionsSettings s;
                    s.sound = ReadSound(e["sound"]);
                    s.tickChance = e.value("tick_chance", 0.0);
                    if (!s.sound.empty()) a.additions.push_back(s);
                };
                if (add.is_array()) { for (const auto& e : add) readOne(e); }
                else readOne(add);
            }
            return a;
        }

        Layer ReadLayer(const fs::path& file) {
            Layer layer;
            std::ifstream in(file, std::ios::binary);
            if (!in) return layer;
            nlohmann::json root;
            try {
                in >> root;
            } catch (const std::exception& e) {
                Log::Warning("[Sound] Unreadable %s: %s", file.string().c_str(), e.what());
                return layer;
            }
            if (!root.is_object() || !root.contains("attributes") || !root["attributes"].is_object()) return layer;
            const nlohmann::json& attrs = root["attributes"];
            if (attrs.contains("minecraft:audio/background_music")) {
                layer.music = ReadBackgroundMusic(attrs["minecraft:audio/background_music"]);
            }
            if (attrs.contains("minecraft:audio/ambient_sounds")) {
                layer.ambient = ReadAmbientSounds(attrs["minecraft:audio/ambient_sounds"]);
            }
            if (attrs.contains("minecraft:audio/music_volume") && attrs["minecraft:audio/music_volume"].is_number()) {
                layer.musicVolume = attrs["minecraft:audio/music_volume"].get<float>();
            }
            return layer;
        }

        fs::path DataRoot() {
            // PlatformMain pins MC_DATA_ROOT to the shipped data/ before the
            // world starts (the terrain library reads it too).
            if (const char* env = std::getenv("MC_DATA_ROOT")) return fs::path(env);
            return fs::path("data");
        }

        const Layer& BiomeLayer(Game::BiomeId biome) {
            static const Layer empty{};
            return biome < g_biomes.size() ? g_biomes[biome] : empty;
        }

        const Layer& DimensionLayer(Game::DimensionId dimension) {
            return g_dimensions[static_cast<size_t>(Game::DimensionSlot(dimension))];
        }

    } // namespace

    void Load() {
        if (g_loaded) return;
        g_loaded = true;
        const fs::path root = DataRoot();

        const Game::BiomeId count = Game::BiomeRegistry::Count();
        g_biomes.assign(count, Layer{});
        int withAudio = 0;
        for (Game::BiomeId id = 0; id < count; ++id) {
            const std::string_view name = Game::BiomeRegistry::Get(id).name;
            const size_t colon = name.find(':');
            const std::string ns = colon == std::string_view::npos ? "minecraft" : std::string(name.substr(0, colon));
            const std::string path = std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
            g_biomes[id] = ReadLayer(root / ns / "worldgen" / "biome" / (path + ".json"));
            if (g_biomes[id].music || g_biomes[id].ambient || g_biomes[id].musicVolume) ++withAudio;
        }

        const Layer overworld = ReadLayer(root / "minecraft" / "dimension_type" / "overworld.json");
        g_dimensions[Game::DimensionSlot(Game::DimensionId::Overworld)] = overworld;
        g_dimensions[Game::DimensionSlot(Game::DimensionId::Nether)] =
            ReadLayer(root / "minecraft" / "dimension_type" / "the_nether.json");
        g_dimensions[Game::DimensionSlot(Game::DimensionId::End)] =
            ReadLayer(root / "minecraft" / "dimension_type" / "the_end.json");
        // The ported mods' own music is not shipped: their dimensions sound
        // like the Overworld's type (game / creative music, the cave mood).
        g_dimensions[Game::DimensionSlot(Game::DimensionId::TwilightForest)] = overworld;
        g_dimensions[Game::DimensionSlot(Game::DimensionId::Aether)] = overworld;
        // The Hush has no dimension type file. Its base: no music of its own
        // (each Hush biome names its songs) and the cave mood, as MC's
        // AmbientSounds.LEGACY_CAVE_SETTINGS.
        {
            Layer hush;
            hush.music = BackgroundMusic{};
            AmbientSounds ambient;
            ambient.mood = AmbientMoodSettings{"ambient.cave", 6000, 8, 2.0};
            hush.ambient = ambient;
            g_dimensions[Game::DimensionSlot(Game::DimensionId::Hush)] = hush;
        }
        Log::Info("[Sound] Audio attributes: %d of %d biomes set music or ambience (data root %s)",
                  withAudio, static_cast<int>(count), root.string().c_str());
    }

    const BackgroundMusic& BackgroundMusicAt(Game::DimensionId dimension, Game::BiomeId biome) {
        Load();
        if (const Layer& b = BiomeLayer(biome); b.music) return *b.music;
        if (const Layer& d = DimensionLayer(dimension); d.music) return *d.music;
        return g_noMusic;
    }

    const AmbientSounds& AmbientSoundsAt(Game::DimensionId dimension, Game::BiomeId biome) {
        Load();
        if (const Layer& b = BiomeLayer(biome); b.ambient) return *b.ambient;
        if (const Layer& d = DimensionLayer(dimension); d.ambient) return *d.ambient;
        return g_noAmbience;
    }

    float MusicVolumeAt(Game::DimensionId dimension, Game::BiomeId biome) {
        Load();
        if (const Layer& b = BiomeLayer(biome); b.musicVolume) return *b.musicVolume;
        if (const Layer& d = DimensionLayer(dimension); d.musicVolume) return *d.musicVolume;
        return 1.0f;
    }

    Game::BiomeId BiomeAt(const glm::dvec3& position) {
        if (!g_clientChunkManager) return Game::kFallbackBiomeId;
        return g_clientChunkManager->BiomeAtWorld(static_cast<int>(std::floor(position.x)),
                                                  static_cast<int>(std::floor(position.y)),
                                                  static_cast<int>(std::floor(position.z)));
    }

} // namespace Client::AudioAttributes
