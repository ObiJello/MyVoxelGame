// File: src/server/commands/WeatherCommand.hpp
// /weather — MC's WeatherCommand.java.
// Usage: /weather clear [<duration>]
//        /weather rain [<duration>]
//        /weather thunder [<duration>]
//   • <duration> is MC's TimeArgument with a minimum of 1 tick ("20s",
//     "0.5d", "6000"); without it the duration is rolled from MC's own
//     distributions: clear from RAIN_DELAY (12000..180000), rain from
//     RAIN_DURATION (12000..24000), thunder from THUNDER_DURATION
//     (3600..15600).
//   • MinecraftServer.setWeatherParameters: clear sets clearWeatherTime and
//     zeroes the rest; rain / thunder set rainTime (and, as vanilla does,
//     thunderTime) to the duration. The server's one WeatherData, so every
//     level that can have weather follows (Server::ServerWeather); the
//     levels ease in over the next ticks exactly as the natural cycle does.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class WeatherCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
