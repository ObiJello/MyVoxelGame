// File: src/server/commands/SeedCommand.cpp
#include "SeedCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../IntegratedServer.hpp"
#include "common/world/level/World.hpp"
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include "common/core/Log.hpp"
#include <string>

namespace Server {

    void SeedCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("seed", SeedCommand::Execute);
    }

    void SeedCommand::Execute(ServerPlayer& /*sender*/,
                              const std::vector<std::string>& /*args*/,
                              ServerConnection& connection,
                              PlayerSessionManager& /*sessionManager*/) {
        Game::World* world = g_integratedServer ? g_integratedServer->GetWorld() : nullptr;
        if (!world) {
            connection.SendChatMessage("Seed is unavailable (no world)", 1);
            return;
        }

        const int64_t seed = world->GetGenerationSeed();

        // MC SeedCommand.java:
        //   Component seedText = ComponentUtils.copyOnClickText(String.valueOf(seed));
        //   sendSuccess(Component.translatable("commands.seed.success", seedText))
        // copyOnClickText wraps the value in square brackets, colours it green,
        // and attaches ClickEvent.CopyToClipboard + hover "chat.copy.click"
        // (ComponentUtils.java:166-168). Both strings below are the values from
        // assets/lang/en_us.json: "Seed: %s" and "Click to Copy to Clipboard".
        const std::string seedText = std::to_string(seed);

        // Note the split: in MC only the NUMBER carries the green colour and the
        // click/hover style. The brackets come from the outer
        // `chat.square_brackets` ("[%s]") translatable, which has default
        // styling — so they render white and are not clickable.
        Network::ChatMessageS2CPacket packet;
        packet.position = 1;  // system message
        packet.segments.push_back(
            Network::ChatSegmentData{"Seed: ", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
        packet.segments.push_back(
            Network::ChatSegmentData{"[", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
        packet.segments.push_back(
            Network::ChatSegmentData{seedText,
                                     0xFF55FF55,  // ChatFormatting.GREEN
                                     Network::ChatClickAction::CopyToClipboard,
                                     seedText,
                                     "Click to Copy to Clipboard"});
        packet.segments.push_back(
            Network::ChatSegmentData{"]", 0xFFFFFFFF, Network::ChatClickAction::None, "", ""});
        connection.SendChatMessage(packet);

        // Imported Minecraft saves never get SetGenerationSeed called (the
        // launch path only seeds procedurally-created worlds, and level.dat
        // isn't parsed), so the number above is the generation-config default
        // rather than the save's real seed. Say so instead of quietly lying.
        if (g_integratedServer->GetConfig().useMinecraftSave) {
            connection.SendChatMessage(
                "Note: this world was imported from a Minecraft save, whose seed isn't read yet - "
                "the value above is this engine's generator default.", 1);
        }

        Log::Info("[SeedCommand] Reported world seed %lld", static_cast<long long>(seed));
    }

} // namespace Server
