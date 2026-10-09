// File: src/server/level/ServerWeather.hpp
//
// MC's weather, server side:
//
//   WeatherData (MinecraftServer.weatherData) — clearWeatherTime, rainTime,
//     thunderTime, raining, thundering: ONE for the whole server, saved in
//     level.dat (clearWeatherTime / rainTime / raining / thunderTime /
//     thundering — this save's DataVersion predates 26.3's weather.dat, and
//     LevelDatToSavedDataPreparationFix moves them on upgrade).
//   ServerLevel.advanceWeatherCycle — the natural cycle (the advance_weather
//     rule) and each weather level's eased rain / thunder levels
//     (World::StepWeatherLevels), with the client game events.
//   ServerLevel.prepareWeather — a level created while it rains starts wet.
//   ServerLevel.resetWeatherCycle — a night slept through clears the rain.
//   MinecraftServer.setWeatherParameters — /weather.
//   ServerLevel.tickThunder — the storm's strikes near players (lightning
//     rods draw them, a skeleton-horse trap on harder difficulties).
//   PlayerList.sendLevelInfo — the weather of the level a player joins or
//     arrives in.
//
// Which levels have weather: Game::DimensionCanHaveWeather. They all share
// the one WeatherData; the cycle ADVANCES once per tick (MC's Overworld; the
// Aether mod's AetherLevelData stops its own advance so the Overworld's
// drives it — the same rule for every shared level here) and every weather
// level eases its own levels toward it.
//
// Server thread only.
#pragma once

#include "common/core/JavaRandom.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace Game { class World; }

namespace Server {

    class IntegratedServer;
    class ServerLevel;

    // MC WeatherData.
    struct WeatherData {
        int  clearWeatherTime = 0;
        int  rainTime         = 0;
        int  thunderTime      = 0;
        bool raining          = false;
        bool thundering       = false;
    };

    class ServerWeather {
    public:
        // MC ServerLevel: UniformInt.of(12000, 180000) / (12000, 24000) and
        // the thunder pair (12000, 180000) / (3600, 15600).
        static constexpr int kRainDelayMin      = 12000;
        static constexpr int kRainDelayMax      = 180000;
        static constexpr int kRainDurationMin   = 12000;
        static constexpr int kRainDurationMax   = 24000;
        static constexpr int kThunderDelayMin   = 12000;
        static constexpr int kThunderDelayMax   = 180000;
        static constexpr int kThunderDurationMin = 3600;
        static constexpr int kThunderDurationMax = 15600;

        ServerWeather();

        // level.dat in / out.
        void Load(const WeatherData& data) { m_data = data; }
        // A 26.3 world's data/minecraft/weather.dat (MC WeatherData.CODEC:
        // clear_weather_time, rain_time, thunder_time, raining, thundering),
        // for a level.dat without the keys. False when absent or unreadable.
        bool LoadSavedDataFile(const std::filesystem::path& file);
        const WeatherData& Data() const { return m_data; }

        // MC ServerLevel.prepareWeather, for a level just created.
        void PrepareLevel(Game::World& world) const;

        // Once per server tick while the simulation runs (MC ServerLevel.tick's
        // "weather" step, before the sleep check): advance the cycle, ease
        // every weather level, send the level changes, and bring every player
        // whose level changed (a join, a dimension change) up to date.
        void Tick(IntegratedServer& server);

        // MC ServerLevel.tickThunder for every chunk of `level`'s ticking set
        // (MC runs it per spawning chunk in entity-ticking range).
        void TickThunder(ServerLevel& level);

        // MC MinecraftServer.setWeatherParameters — note MC's thunderTime is
        // set to rainTime too.
        void SetWeatherParameters(int clearTime, int rainTime, bool raining, bool thundering);

        // MC ServerLevel.resetWeatherCycle (the night skip).
        void ResetWeatherCycle();

        // MC's IntProviders, from the weather's own stream (MC samples from
        // the level's random; /weather from the command source's level's).
        int SampleRainDelay();
        int SampleRainDuration();
        int SampleThunderDuration();

        // A player's client state is to be re-sent (their connection went
        // away, or a forced resync).
        void Forget(uint32_t connectionId) { m_synced.erase(connectionId); }

        // Bring every player whose level changed (a join, a dimension
        // change) up to date — MC PlayerList.sendLevelInfo. Tick() does it
        // while the simulation runs; the server calls it on its own while
        // paused, since a join's level info goes out pause or not.
        void SyncPlayers(IntegratedServer& server);

    private:
        void AdvanceCycle();
        // MC ServerLevel.findLightningTargetAround / findLightningRod.
        glm::ivec3 FindLightningTargetAround(ServerLevel& level, const glm::ivec3& pos);

        WeatherData     m_data;
        Game::JavaRandom m_random{0};
        // Connection id -> the dimension whose weather that client was last
        // brought up to date with (PlayerList.sendLevelInfo).
        std::unordered_map<uint32_t, Game::DimensionId> m_synced;
    };

} // namespace Server
