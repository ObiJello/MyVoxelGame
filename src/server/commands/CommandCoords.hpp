// File: src/server/commands/CommandCoords.hpp
//
// MC net.minecraft.commands.arguments.coordinates.{Vec3Argument, WorldCoordinate}
// — the `<x> <y> <z>` grammar every positional command shares.
//
// Its own file for the same reason EntitySelector is: in MC a coordinate
// triple is an ARGUMENT TYPE, not a feature of one command. /tp, /summon,
// /setblock, /spawnpoint and /execute positioned all take the identical
// grammar, and writing the parser inside whichever command needed it first
// guarantees the second one reimplements a subset and the two disagree about
// `~`, about centre-correction, or about whether `^` may be mixed with `~`.
//
// This lived in an anonymous namespace inside TeleportCommand.cpp until
// /summon needed coordinates too.
#pragma once

#include "EntitySelector.hpp"   // CommandSource

#include <glm/glm.hpp>
#include <string>

namespace Server {

    // The source's facing, for the local `^` frame. MC's convention — yaw 0 is
    // +Z (south), pitch positive DOWN.
    struct CommandRotation {
        float yRot = 0.0f;
        float xRot = 0.0f;
    };

    // MC WorldCoordinate.parseDouble — "~", "~5", "~-3", "100", "100.5".
    //
    // `centerCorrect` is MC's block-position flag: an integer coordinate in a
    // Vec3Argument means the CENTRE of that block, which is why `/tp 100 64 100`
    // lands on 100.5 and not on a corner. A coordinate written WITH a decimal
    // point is taken literally.
    bool ParseCoord(const std::string& arg, double origin, bool centerCorrect,
                    double& out);

    // Could this token START a coordinate? Used to tell `/tp <dest>` from
    // `/tp <x> <y> <z>` before either has been parsed.
    bool LooksLikeCoord(const std::string& s);

    // MC Vec3Argument: a triple is EITHER three world coordinates (absolute or
    // `~`) or three LOCAL `^` ones. Mixing them is ERROR_MIXED_TYPE in vanilla,
    // and rightly so — the local frame is a basis, and two thirds of a basis
    // means nothing.
    //
    // `^left ^up ^forward` is measured in the SOURCE's own frame, so `^ ^ ^5`
    // is five blocks along the way they are looking, pitch included.
    bool ParseVec3(const std::string& ax, const std::string& ay, const std::string& az,
                   const CommandSource& src, const CommandRotation& srcRot,
                   glm::dvec3& out, std::string& error);

} // namespace Server
