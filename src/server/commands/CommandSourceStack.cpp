// File: src/server/commands/CommandSourceStack.cpp
#include "CommandSourceStack.hpp"
#include "../IntegratedServer.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include "common/world/level/GameRules.hpp"

#include <ctime>

namespace Server {

    CommandSourceStack CommandSourceStack::ForPlayer(ServerPlayer& sender, PlayerSessionManager& sessions) {
        CommandSourceStack s;
        s.sender   = &sender;
        s.sessions = &sessions;
        s.position = sender.getPosition();
        // The session's dimension — selectors are level-scoped (see
        // CommandSource); the player's own field is the fallback.
        if (auto session = sessions.GetSession(sender.getPlayerId())) {
            s.dimension = Game::DimensionFromRaw(session->GetDimensionId());
        } else {
            s.dimension = Game::DimensionFromRaw(sender.getDimensionId());
        }
        s.rotation = CommandRotation{ sender.getYaw(), sender.getPitch() };
        SelectedEntity self;
        if (DescribePlayer(sender, s, self)) s.entity = std::move(self);
        s.anchorEyes = false;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithEntity(const SelectedEntity& entity) const {
        CommandSourceStack s = *this;
        s.entity = entity;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithPosition(const glm::dvec3& position) const {
        CommandSourceStack s = *this;
        s.position = position;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithRotation(const CommandRotation& rot) const {
        CommandSourceStack s = *this;
        s.rotation = rot;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithLevel(Game::DimensionId dimension) const {
        CommandSourceStack s = *this;
        s.dimension = dimension;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithAnchor(bool eyes) const {
        CommandSourceStack s = *this;
        s.anchorEyes = eyes;
        return s;
    }

    CommandSourceStack CommandSourceStack::Facing(const glm::dvec3& target) const {
        // MC: dx/dy/dz from the anchor, xRot = -atan2(dy, horizontal),
        // yRot = atan2(dz, dx) - 90 — the same angles Mth's helpers derive.
        const glm::vec3 d(target - AnchorPosition());
        return WithRotation(CommandRotation{ Game::Mth::YRotFromVector(d), Game::Mth::XRotFromVector(d) });
    }

    CommandSourceStack CommandSourceStack::Facing(const SelectedEntity& entity, bool eyes) const {
        return Facing(EntityAnchorPosition(entity, eyes));
    }

    // ── Output ─────────────────────────────────────────────────────────────

    namespace {

        std::string PlainText(const Network::ChatMessageS2CPacket& message) {
            std::string text;
            for (const auto& segment : message.segments) text += segment.text;
            return text;
        }

        // MC CloseableCommandBlockSource.TIME_FORMAT: "[HH:mm:ss] ", local time.
        std::string CommandBlockTimestamp() {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "[%H:%M:%S] ", &local);
            return buf;
        }

        // MC PlayerList.isOp for this engine: a player who may run commands
        // here — the host, or a guest while the world grants guests command
        // access — in a world that allows commands (ServerConnection's chat
        // gate is the same test).
        bool IsCommandOperator(ServerConnection& connection) {
            IntegratedServer* server = g_integratedServer.get();
            if (!server || !server->IsAllowCommands()) return false;
            return connection.IsSingleplayerOwner() || server->GetGuestCommandAccess();
        }

        // MC "chat.type.admin" in GRAY (italic has no wire form here).
        constexpr uint32_t kAdminColor = 0xFFAAAAAAu;

    } // namespace

    std::string CommandSourceStack::DisplayName() const {
        if (entity && !entity->name.empty()) return entity->name;
        if (sender) return sender->getName();
        return "@";   // MC BaseCommandBlock's default name
    }

    void CommandSourceStack::SendSuccess(ServerConnection& connection, const std::string& message,
                                         bool broadcast) const {
        SendSuccess(connection, Network::ChatMessageS2CPacket(message, 1), broadcast);
    }

    void CommandSourceStack::SendSuccess(ServerConnection& connection,
                                         const Network::ChatMessageS2CPacket& message,
                                         bool broadcast) const {
        if (silent) return;
        using Game::Rules::Id;
        // acceptsSuccess: ServerPlayer's command source and
        // CloseableCommandBlockSource both answer send_command_feedback.
        if (Game::Rules::GetBool(Id::SendCommandFeedback)) {
            if (commandBlockOutput) {
                *commandBlockOutput = CommandBlockTimestamp() + PlainText(message);
            } else {
                Network::ChatMessageS2CPacket packet = message;
                packet.position = 1;
                connection.SendChatMessage(packet);
            }
        }
        // shouldInformAdmins: always for a player, command_block_output for a
        // command block.
        if (broadcast && (!commandBlockOutput || Game::Rules::GetBool(Id::CommandBlockOutput))) {
            BroadcastToAdmins(PlainText(message));
        }
    }

    void CommandSourceStack::BroadcastToAdmins(const std::string& message) const {
        const std::string text = "[" + DisplayName() + ": " + message + "]";
        using Game::Rules::Id;
        // Every operator online but the source itself (a player source; a
        // command block is nobody's), under send_command_feedback.
        if (Game::Rules::GetBool(Id::SendCommandFeedback) && sessions) {
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session) continue;
                ServerPlayer* player = session->GetPlayer();
                ServerConnection* connection = session->GetConnection();
                if (!player || !connection) continue;
                if (!commandBlockOutput && player == sender) continue;
                if (!IsCommandOperator(*connection)) continue;
                Network::ChatMessageS2CPacket packet;
                packet.position = 1;
                packet.segments.push_back(Network::ChatSegmentData{
                    text, kAdminColor, Network::ChatClickAction::None, "", ""});
                connection->SendChatMessage(packet);
            }
        }
        // MC: the server's own system message — its console log — under
        // log_admin_commands (the source is never the server itself here).
        if (Game::Rules::GetBool(Id::LogAdminCommands)) {
            Log::Info("[Server] %s", text.c_str());
        }
    }

} // namespace Server
