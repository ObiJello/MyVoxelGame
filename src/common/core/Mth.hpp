// File: src/common/core/Mth.hpp
//
// The subset of MC's net.minecraft.util.Mth that the entity system needs.
//
// Why this exists as its own header rather than being scattered: MC's angle
// helpers are subtly asymmetric — wrapDegrees maps to [-180, 180), rotLerp
// takes the SHORT way round, and rotateIfNecessary clamps a value to within N
// degrees of an anchor rather than lerping toward it. Reimplementing any of
// them slightly differently at each call site is how mob heads end up spinning
// the long way round once per revolution. There is one copy, and it matches
// Mth.java line for line.
//
// A near-duplicate of Wrap180/RotLerp already lives in
// src/client/entity/RemotePlayerManager.hpp. That one is left alone
// deliberately: it is client render code with its own float conventions, and
// unifying it is a separate change with its own regression surface.
#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace Game::Mth {

    inline constexpr float  kPi        = 3.14159265358979323846f;
    inline constexpr float  kDegToRad  = kPi / 180.0f;
    inline constexpr float  kRadToDeg  = 180.0f / kPi;

    template <typename T>
    inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

    inline float Lerp(float t, float a, float b) { return a + t * (b - a); }
    inline double Lerp(double t, double a, double b) { return a + t * (b - a); }

    // Mth.wrapDegrees — fold an angle into [-180, 180).
    inline float WrapDegrees(float deg) {
        float r = std::fmod(deg, 360.0f);
        if (r >= 180.0f)  r -= 360.0f;
        if (r < -180.0f)  r += 360.0f;
        return r;
    }

    inline double WrapDegrees(double deg) {
        double r = std::fmod(deg, 360.0);
        if (r >= 180.0)  r -= 360.0;
        if (r < -180.0)  r += 360.0;
        return r;
    }

    // Mth.degreesDifference — signed shortest rotation from `from` to `to`.
    inline float DegreesDifference(float from, float to) {
        return WrapDegrees(to - from);
    }

    // Mth.rotLerp — interpolate an angle the short way round.
    inline float RotLerp(float t, float from, float to) {
        return from + t * DegreesDifference(from, to);
    }

    // Mth.rotateIfNecessary — pull `value` to within `maxDelta` degrees of
    // `anchor`, leaving it untouched when it is already inside that window.
    //
    // NOT a lerp: this is a hard clamp, and it is what keeps a mob's head from
    // exceeding its neck limit while still letting it look freely inside it.
    inline float RotateIfNecessary(float value, float anchor, float maxDelta) {
        const float diff = DegreesDifference(value, anchor);
        const float clamped = Clamp(diff, -maxDelta, maxDelta);
        return anchor - clamped;
    }

    // Mth.approachDegrees — step `from` toward `to` by at most `maxDelta`.
    inline float ApproachDegrees(float from, float to, float maxDelta) {
        return from + Clamp(DegreesDifference(from, to), -maxDelta, maxDelta);
    }

    // Mth.positiveCeilDiv — ceiling division for non-negative operands. Used by
    // Goal.reducedTickDelay, which is why a "40 tick" goal delay is really 20
    // when the goal is only evaluated every other tick.
    inline int PositiveCeilDiv(int x, int y) {
        // MC: -Math.floorDiv(-x, y). C++ integer division truncates toward
        // zero rather than flooring, so the floor has to be spelled out.
        const int a = -x, b = y;
        int q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;   // floorDiv
        return -q;
    }

    // ── Rotation wire encoding (Mth.packDegrees / unpackDegrees) ───────────
    //
    // One byte per angle: 1/256 of a turn, i.e. 1.40625 degrees per step. Both
    // halves must agree exactly or entity heads jitter by up to a step.
    inline int8_t PackDegrees(float deg) {
        // MC casts the floored int straight to byte, which is a truncation to
        // the low 8 bits with wraparound — go through uint8_t so the narrowing
        // is well defined rather than implementation-defined.
        const int floored = static_cast<int>(std::floor(deg * 256.0f / 360.0f));
        return static_cast<int8_t>(static_cast<uint8_t>(floored & 0xFF));
    }

    inline float UnpackDegrees(int8_t packed) {
        return static_cast<float>(packed) * 360.0f / 256.0f;
    }

    // Length of a 3D vector, spelled out so call sites read like MC's.
    inline double Length(double x, double y, double z) {
        return std::sqrt(x * x + y * y + z * z);
    }

    // ── Look angles ⇄ direction vectors ─────────────────────────────────────
    //
    // THE convention, everywhere in this engine, for players and mobs alike:
    //
    //   yRot (yaw)   degrees, 0 = facing +Z (south), increasing CLOCKWISE
    //                seen from above, so 90 = -X (west), 180 = -Z (north),
    //                270 = +X (east).
    //   xRot (pitch) degrees, POSITIVE LOOKING DOWN. -90 is straight up.
    //
    // That is Minecraft's convention, and it is not arbitrary: every angle that
    // crosses the wire, every mob's head rotation, every `facing=` blockstate
    // and every /tp argument is defined against it. The renderer used to keep a
    // second convention for the camera (yaw 0 = +X, pitch positive up) and
    // convert at each boundary, which meant every new feature touching angles
    // had to rediscover which side of the boundary it was on.
    //
    // These four functions are the only place the trigonometry lives. Deriving
    // a direction by hand at a call site is what re-introduces the split.

    // MC Entity.calculateViewVector(xRot, yRot). Unit length.
    inline glm::vec3 ViewVector(float xRot, float yRot) {
        const float x = xRot * kDegToRad;
        const float y = -yRot * kDegToRad;
        const float cosY = std::cos(y), sinY = std::sin(y);
        const float cosX = std::cos(x), sinX = std::sin(x);
        return glm::vec3(sinY * cosX, -sinX, cosY * cosX);
    }

    // The same with pitch forced flat — MC Entity.getLookAngle() projected, and
    // what WASD movement and body orientation use.
    inline glm::vec3 HorizontalViewVector(float yRot) {
        const float y = -yRot * kDegToRad;
        return glm::vec3(std::sin(y), 0.0f, std::cos(y));
    }

    // Inverse of ViewVector — MC Entity.lookAt's two atan2 expressions.
    // `dir` need not be normalised.
    inline float YRotFromVector(const glm::vec3& dir) {
        return WrapDegrees(std::atan2(dir.z, dir.x) * kRadToDeg - 90.0f);
    }

    inline float XRotFromVector(const glm::vec3& dir) {
        const float horizontal = std::sqrt(dir.x * dir.x + dir.z * dir.z);
        return WrapDegrees(-(std::atan2(dir.y, horizontal) * kRadToDeg));
    }

} // namespace Game::Mth
