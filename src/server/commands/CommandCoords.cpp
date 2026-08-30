// File: src/server/commands/CommandCoords.cpp
#include "CommandCoords.hpp"

#include "common/core/Mth.hpp"

#include <cctype>
#include <cmath>
#include <string>

namespace Server {

    // MC WorldCoordinate.parseDouble — "~", "~5", "~-3", "100", "100.5".
    // `centerCorrect` is MC's flag for the block-position form: an integer
    // coordinate in a Vec3Argument means the CENTRE of that block, which is
    // why `/tp 100 64 100` lands you at 100.5 / 100.5 and not on a corner.
    bool ParseCoord(const std::string& arg, double origin, bool centerCorrect,
                    double& out) {
        if (arg.empty()) return false;

        if (arg[0] == '~') {
            if (arg.size() == 1) { out = origin; return true; }
            try {
                size_t used = 0;
                const double d = std::stod(arg.substr(1), &used);
                if (used != arg.size() - 1) return false;
                out = origin + d;
                return true;
            } catch (...) { return false; }
        }

        try {
            size_t used = 0;
            const double v = std::stod(arg, &used);
            if (used != arg.size()) return false;
            // Only a coordinate written WITHOUT a decimal point is centred.
            out = (centerCorrect && arg.find('.') == std::string::npos) ? v + 0.5 : v;
            return true;
        } catch (...) { return false; }
    }

    bool LooksLikeCoord(const std::string& s) {
        if (s.empty()) return false;
        if (s[0] == '~' || s[0] == '^') return true;
        if (s[0] == '@') return false;
        size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
        if (i >= s.size()) return false;
        return std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.';
    }

    // One `^n` component. Bare `^` is zero, like bare `~`.
    bool ParseLocalComponent(const std::string& arg, double& out) {
        if (arg.empty() || arg[0] != '^') return false;
        if (arg.size() == 1) { out = 0.0; return true; }
        try {
            size_t used = 0;
            out = std::stod(arg.substr(1), &used);
            return used == arg.size() - 1;
        } catch (...) { return false; }
    }

    // MC Vec3Argument: a coordinate triple is EITHER three world
    // coordinates (absolute or `~`) or three LOCAL `^` ones. Mixing them is
    // ERROR_MIXED_TYPE in vanilla, and rightly so — the local frame is a
    // basis, and two thirds of a basis means nothing.
    //
    // `^left ^up ^forward` is measured in the SOURCE's own frame, so
    // `^ ^ ^5` is five blocks along the way they are looking, pitch
    // included. That is the whole point of it over `~`.
    bool ParseVec3(const std::string& ax, const std::string& ay, const std::string& az,
                   const CommandSource& src, const CommandRotation& srcRot,
                   glm::dvec3& out, std::string& error) {
        const bool lx = !ax.empty() && ax[0] == '^';
        const bool ly = !ay.empty() && ay[0] == '^';
        const bool lz = !az.empty() && az[0] == '^';

        if (lx || ly || lz) {
            if (!(lx && ly && lz)) {
                error = "Cannot mix world & local coordinates "
                        "(everything must either use ^ or not)";
                return false;
            }
            double left = 0.0, up = 0.0, forwards = 0.0;
            if (!ParseLocalComponent(ax, left) ||
                !ParseLocalComponent(ay, up) ||
                !ParseLocalComponent(az, forwards)) {
                error = "Invalid local coordinate: " + ax + " " + ay + " " + az;
                return false;
            }

            // MC Vec3.applyLocalCoordinatesToRotation. `up` is the view
            // vector pitched a further quarter turn up, so the frame tilts
            // with the head instead of staying world-aligned — which is why
            // `^ ^1 ^` while looking down moves you FORWARD-and-down's
            // perpendicular, not straight up.
            const glm::dvec3 forward =
                glm::dvec3(Game::Mth::ViewVector(srcRot.xRot, srcRot.yRot));
            const glm::dvec3 upVec =
                glm::dvec3(Game::Mth::ViewVector(srcRot.xRot - 90.0f, srcRot.yRot));
            const glm::dvec3 leftVec = -glm::cross(forward, upVec);

            // MC's anchor here is the source's, which defaults to FEET.
            out = src.position + forward * forwards + upVec * up + leftVec * left;
            return true;
        }

        if (!ParseCoord(ax, src.position.x, true,  out.x) ||
            !ParseCoord(ay, src.position.y, false, out.y) ||
            !ParseCoord(az, src.position.z, true,  out.z)) {
            error = "Invalid position: " + ax + " " + ay + " " + az;
            return false;
        }
        return true;
    }


} // namespace Server
