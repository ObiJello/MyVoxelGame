// File: src/server/commands/CommandSourceStack.hpp
//
// MC net.minecraft.commands.CommandSourceStack — everything a command reads
// about WHO runs it and FROM WHERE, reduced to what this engine has.
//
// A plain `/tp ~ ~5 ~` builds one from its sender (ForPlayer). `/execute`
// is the reason the type exists: every one of its subcommands is a
// transformation of the stack (`as` swaps the entity, `at` the level,
// position and rotation, `positioned` the position, `anchored` the anchor…)
// and the final `run` hands the transformed stack to an ordinary command,
// which must therefore read its origin from the stack and never from the
// sender's player. The sender stays the sender throughout: feedback goes to
// them, and `/kill`'s sender-sparing rule still knows who they are.
#pragma once

#include "EntitySelector.hpp"   // CommandSource, SelectedEntity
#include "CommandCoords.hpp"    // CommandRotation

#include <glm/glm.hpp>

#include <string>

namespace Network { struct ChatMessageS2CPacket; }

namespace Server {

    class ServerConnection;

    struct CommandSourceStack : CommandSource {
        // MC CommandSourceStack.rotation — MC's convention (yaw 0 = +Z,
        // pitch positive down), the source of `~` in a rotation argument and
        // of the `^` frame.
        CommandRotation rotation;

        // The stack a player's own command starts from: their session's
        // dimension, feet position, view rotation, themselves as the entity,
        // FEET anchor.
        static CommandSourceStack ForPlayer(ServerPlayer& sender, PlayerSessionManager& sessions);

        // MC's with* / facing builders. Each returns a modified copy.
        CommandSourceStack WithEntity(const SelectedEntity& entity) const;
        CommandSourceStack WithPosition(const glm::dvec3& position) const;
        CommandSourceStack WithRotation(const CommandRotation& rotation) const;
        CommandSourceStack WithLevel(Game::DimensionId dimension) const;
        CommandSourceStack WithAnchor(bool eyes) const;
        // MC CommandSourceStack.facing(Vec3): rotate so the ANCHOR looks at
        // the point.
        CommandSourceStack Facing(const glm::dvec3& target) const;
        CommandSourceStack Facing(const SelectedEntity& entity, bool eyes) const;

        // MC CommandSourceStack.getPlayer(): the EXECUTOR when it is a player
        // (the sender for a plain command; `/execute as` changes it), else
        // null. Commands that act on "you" read this, never the sender —
        // `/execute as Steve run gamemode creative` changes Steve.
        ServerPlayer* ExecutorPlayer() const {
            return entity && entity->kind == SelectedEntity::Kind::Player ? entity->player : nullptr;
        }
        // MC permissions.requires.player — getPlayerOrException's message.
        static constexpr const char* kPlayerRequired = "A player is required to run this command here";

        // MC getAnchor().apply(this).
        glm::dvec3 AnchorPosition() const { return SourceAnchorPosition(*this); }

        // ── Output (MC CommandSourceStack.source / silent) ─────────────────
        //
        // WHO hears the command. A player's own command (and everything
        // `/execute` forks from it) answers the sender over `connection`. A
        // command block minecart's run points this at the cart's output: its
        // successes become the cart's last output (MC CloseableCommandBlock-
        // Source.sendSystemMessage) instead of anyone's chat.
        std::string* commandBlockOutput = nullptr;
        // MC CommandSourceStack.silent — nothing is said at all.
        bool silent = false;

        // MC CommandSourceStack.sendSuccess(message, broadcast). The source
        // hears it when it acceptsSuccess — the send_command_feedback rule,
        // for a player and a command block alike — and with `broadcast` (MC's
        // "allowLogging": a command that changed something) the operators
        // are told "[<source>: <message>]" when the source shouldInformAdmins
        // (always for a player; command_block_output for a command block):
        // the other operators online under send_command_feedback, the server
        // log under log_admin_commands. Every command's success line goes
        // through here; an error goes straight to the connection (MC
        // sendFailure, which no rule gates).
        void SendSuccess(ServerConnection& connection, const std::string& message, bool broadcast) const;
        void SendSuccess(ServerConnection& connection, const Network::ChatMessageS2CPacket& message,
                         bool broadcast) const;

        // MC getDisplayName(): the entity running the command (`/execute as`
        // renames the source), else the sender.
        std::string DisplayName() const;

    private:
        // MC CommandSourceStack.broadcastToAdmins.
        void BroadcastToAdmins(const std::string& message) const;
    };

} // namespace Server
