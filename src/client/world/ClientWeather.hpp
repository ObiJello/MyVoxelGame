// File: src/client/world/ClientWeather.hpp
//
// The client's half of MC's weather: ClientLevel's rain and thunder levels
// (Level.oRainLevel / rainLevel / oThunderLevel / thunderLevel) and the
// queries every client system asks of them.
//
// The server owns the weather (Server::ServerWeather) and tells the client
// through GameEventS2CPacket — MC ClientPacketListener.handleGameEvent:
//   START_RAINING         setRainLevel(0)
//   STOP_RAINING          setRainLevel(1)
//   RAIN_LEVEL_CHANGE     setRainLevel(param)
//   THUNDER_LEVEL_CHANGE  setThunderLevel(param)
// setX writes BOTH ends of the lerp (MC Level.setRainLevel), so the level
// steps 0.01 per server tick with no inter-tick smoothing — as vanilla.
//
// One set of levels: the weather of the levels that can have it, which all
// share the server's one WeatherData (ServerWeather). The server keeps every
// player told — a player in a weather level gets that level's levels, one
// anywhere else the Overworld's — so a portal view INTO a weather level
// (drawn from the Nether, say) shows its rain. A dimension change builds a
// new ClientLevel in MC (dry), so Reset() runs on ChangeDimensionS2C and at
// session start, and the server re-sends after it. Every query answers for
// the BOUND level (ClientLevels::BoundDimension — the active one, or the far
// level while a portal view draws it), and a dimension that cannot have
// weather (Game::DimensionCanHaveWeather) always reads dry.
//
// Main thread only (the packet is applied in order on the main thread).
// EnvironmentState mirrors the levels into the sky / fog / lightmap each
// frame; the rain renderer, the splashes, the rain sounds and the client
// level bridge read them here.
#pragma once

#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Game { struct IBlockAccess; }

namespace Client::ClientWeather {

    // GameEventS2CPacket's weather events (MC handleGameEvent).
    void OnGameEvent(uint8_t event, float param);

    // A new ClientLevel: dry, no thunder.
    void Reset();

    // MC Level.canHaveWeather for the bound dimension.
    bool CanHaveWeather();

    // The levels as `dimension` sees them (0 where it cannot have weather).
    float RainLevelIn(Game::DimensionId dimension, float partialTick = 1.0f);
    float ThunderLevelIn(Game::DimensionId dimension, float partialTick = 1.0f);

    // MC Level.getRainLevel(a) / getThunderLevel(a) (thunder x rain). 0 in
    // a dimension that cannot have weather.
    float RainLevel(float partialTick = 1.0f);
    float ThunderLevel(float partialTick = 1.0f);

    // MC Level.isRaining (rain > 0.2) / isThundering (thunder > 0.9).
    bool IsRaining();
    bool IsThundering();

    // MC Level.getHeight(MOTION_BLOCKING, x, z): the first free Y above the
    // column's top motion-blocking block (collision or fluid) in the bound
    // level's client chunk; the dimension's minimum Y where no chunk is.
    int MotionBlockingHeight(int worldX, int worldZ);

    // MC Level.precipitationAt(pos) on the client: NONE unless raining; NONE
    // where the sky light is below 15 (canSeeSky) or the cell is under the
    // MOTION_BLOCKING heightmap; else the biome's precipitation at that
    // height. Values are Game::BiomeRegistry::Precipitation's (0 none,
    // 1 rain, 2 snow).
    int PrecipitationAt(const Game::IBlockAccess& blocks, const glm::ivec3& pos);

    // MC ClientLevel.getPrecipitationAt(pos) — a different rule from the
    // one above: NONE where no chunk is loaded, otherwise the biome's
    // precipitation at that height, with no weather, sky or heightmap test.
    // What WeatherEffectRenderer (at the camera's height) and
    // tickWeatherEffects ask; they gate on the rain level themselves.
    int BiomePrecipitationAt(const Game::IBlockAccess& blocks, const glm::ivec3& pos);

} // namespace Client::ClientWeather
