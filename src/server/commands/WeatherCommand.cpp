// File: src/server/commands/WeatherCommand.cpp
#include "WeatherCommand.hpp"
#include "TimeCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerWeather.hpp"
#include "../network/ServerConnection.hpp"
#include "common/core/Log.hpp"

#include <string>

namespace Server {

    void WeatherCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // MC WeatherCommand: (clear|rain|thunder) [duration: TimeArgument(1)].
        dispatcher.RegisterCommand("weather", WeatherCommand::Execute,
            Cmd::Root()
                .Then(Cmd::Literal("clear").Executes()
                    .Then(Cmd::Argument("duration", Cmd::Arg::Time).Executes()))
                .Then(Cmd::Literal("rain").Executes()
                    .Then(Cmd::Argument("duration", Cmd::Arg::Time).Executes()))
                .Then(Cmd::Literal("thunder").Executes()
                    .Then(Cmd::Argument("duration", Cmd::Arg::Time).Executes())));
    }

    void WeatherCommand::Execute(const CommandSourceStack& source,
                                 const std::vector<std::string>& args,
                                 ServerConnection& connection,
                                 PlayerSessionManager& /*sessionManager*/) {
        ServerWeather* weather = g_integratedServer ? g_integratedServer->Weather() : nullptr;
        if (!weather) {
            connection.SendChatMessage("Weather is unavailable (no world)", 1);
            return;
        }
        if (args.empty() || args.size() > 2) {
            connection.SendChatMessage("Usage: /weather <clear|rain|thunder> [<duration>]", 1);
            return;
        }
        const std::string& kind = args[0];
        if (kind != "clear" && kind != "rain" && kind != "thunder") {
            connection.SendChatMessage("Usage: /weather <clear|rain|thunder> [<duration>]", 1);
            return;
        }

        // DEFAULT_TIME (-1) means "roll MC's distribution" (getDuration).
        int duration = -1;
        if (args.size() == 2) {
            std::string error;
            if (!TimeCommand::ParseTimeArgumentTicks(args[1], 1, duration, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
        }

        if (kind == "clear") {
            // setClear: setWeatherParameters(getDuration(RAIN_DELAY), 0, false, false).
            weather->SetWeatherParameters(duration == -1 ? weather->SampleRainDelay() : duration, 0,
                                          false, false);
            source.SendSuccess(connection, "Set the weather to clear", true);
        } else if (kind == "rain") {
            // setRain: setWeatherParameters(0, getDuration(RAIN_DURATION), true, false).
            weather->SetWeatherParameters(0, duration == -1 ? weather->SampleRainDuration() : duration,
                                          true, false);
            source.SendSuccess(connection, "Set the weather to rain", true);
        } else {
            // setThunder: setWeatherParameters(0, getDuration(THUNDER_DURATION), true, true).
            weather->SetWeatherParameters(0, duration == -1 ? weather->SampleThunderDuration() : duration,
                                          true, true);
            source.SendSuccess(connection, "Set the weather to rain & thunder", true);
        }
        Log::Info("[Weather] /weather %s%s%s", kind.c_str(), args.size() == 2 ? " " : "",
                  args.size() == 2 ? args[1].c_str() : "");
    }

} // namespace Server
