// File: src/common/core/Ease.hpp
//
// MC net.minecraft.util.Ease — the easing curves the 26.x item animations
// (SpearAnimations, the swing types) are written in. Transcribed function
// for function, float where MC is float and double where MC promotes, so the
// curves land on the same values.
#pragma once

#include <algorithm>
#include <cmath>

namespace Game::Ease {

    inline float Square(float x) { return x * x; }
    inline float Cube(float x) { return x * x * x; }

    inline float InQuad(float x) { return x * x; }
    inline float OutQuad(float x) { return 1.0f - Square(1.0f - x); }
    inline float InCubic(float x) { return Cube(x); }
    inline float OutCubic(float x) { return 1.0f - Cube(1.0f - x); }
    inline float OutQuart(float x) { return 1.0f - Square(Square(1.0f - x)); }

    inline float InSine(float x) { return 1.0f - std::cos(x * 1.5707964f); }
    inline float OutSine(float x) { return std::sin(x * 1.5707964f); }
    inline float InOutSine(float x) { return -(std::cos(3.1415927f * x) - 1.0f) / 2.0f; }

    inline float InBack(float x) { return Square(x) * (2.70158f * x - 1.70158f); }
    inline float OutBack(float x) {
        return 1.0f + 2.70158f * Cube(x - 1.0f) + 1.70158f * Square(x - 1.0f);
    }
    inline float InOutBack(float x) {
        if (x < 0.5f) return 4.0f * x * x * (7.189819f * x - 2.5949094f) / 2.0f;
        const float dt = 2.0f * x - 2.0f;
        return (dt * dt * (3.5949094f * dt + 2.5949094f) + 2.0f) / 2.0f;
    }

    inline float InCirc(float x) {
        return static_cast<float>(-std::sqrt(static_cast<double>(1.0f - x * x))) + 1.0f;
    }
    inline float OutCirc(float x) {
        return static_cast<float>(std::sqrt(static_cast<double>(1.0f - Square(x - 1.0f))));
    }

    inline float InExpo(float x) {
        return x == 0.0f ? 0.0f : static_cast<float>(std::pow(2.0, 10.0 * x - 10.0));
    }
    inline float OutExpo(float x) {
        return x == 1.0f ? 1.0f : 1.0f - static_cast<float>(std::pow(2.0, -10.0 * x));
    }
    inline float InOutExpo(float x) {
        if (x < 0.5f) return x == 0.0f ? 0.0f : static_cast<float>(std::pow(2.0, 20.0 * x - 10.0) / 2.0);
        return x == 1.0f ? 1.0f : static_cast<float>((2.0 - std::pow(2.0, -20.0 * x + 10.0)) / 2.0);
    }

    inline float InOutElastic(float x) {
        if (x == 0.0f) return 0.0f;
        if (x == 1.0f) return 1.0f;
        const double s = std::sin((20.0 * x - 11.125) * 1.3962634801864624);
        return x < 0.5f ? static_cast<float>(-(std::pow(2.0, 20.0 * x - 10.0) * s) / 2.0)
                        : static_cast<float>(std::pow(2.0, -20.0 * x + 10.0) * s / 2.0 + 1.0);
    }

    // SpearAnimations.progress: Mth.clamp(Mth.inverseLerp(time, start, end), 0, 1).
    inline float Progress(float time, float start, float end) {
        return std::clamp((time - start) / (end - start), 0.0f, 1.0f);
    }

} // namespace Game::Ease
