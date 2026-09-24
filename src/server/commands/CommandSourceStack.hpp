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

namespace Server {

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

        // MC getAnchor().apply(this).
        glm::dvec3 AnchorPosition() const { return SourceAnchorPosition(*this); }
    };

} // namespace Server
