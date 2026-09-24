// File: src/client/renderer/mesh/SectionFade.hpp
//
// MC's chunk-section fade-in (Options.chunkSectionFadeInTime, RenderSection.
// getVisibility): a section that has just had its first mesh uploaded is
// drawn as the fog colour and resolves to its own colours over the option's
// seconds. The clock is one int32 of milliseconds since the first call —
// the value that rides the section's origin-table row (ChunkMegaBuffer,
// ivec4 .w) and the frame's uFadeNowMs uniform, so the shader computes the
// 0..1 itself and no row is touched again while a section fades. Never 0:
// 0 in a row means "never uploaded" and reads as fully visible.
#pragma once

#include <chrono>
#include <cstdint>

namespace Render::SectionFade {

    inline int32_t NowMs() {
        static const auto s_epoch = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s_epoch).count();
        return static_cast<int32_t>(ms) + 1;
    }

    // How recently a chunk's data must have ARRIVED for its sections to fade
    // in when their first mesh uploads. MC stamps uploadedTime on every
    // section's first upload, but it meshes everything in range as it
    // arrives; this engine meshes lazily in view order, so a section of a
    // chunk that arrived long ago can see its first upload only when the
    // player turns to it — and fading that in reads as the world popping in
    // on a head turn. Fresh arrivals (walking into new terrain) still fade;
    // anything first meshed later than this shows at once.
    inline constexpr int32_t kFreshArrivalMs = 2000;

    // A fade start that reads as long finished: the clock's first tick, so
    // every elapsed time is past any fade duration. Never 0 (0 = "never
    // uploaded").
    inline constexpr int32_t kAlreadyVisible = 1;

    // MC RenderSection.getVisibility(now, fadeDuration): elapsed / duration,
    // clamped; 1 with the fade off or for a section never uploaded (an
    // all-air section has no mesh and nothing to fade).
    inline float Visibility(int32_t uploadedMs, int32_t nowMs, int32_t fadeMs) {
        if (fadeMs <= 0 || uploadedMs == 0) return 1.0f;
        const int32_t elapsed = nowMs - uploadedMs;
        if (elapsed >= fadeMs) return 1.0f;
        return elapsed <= 0 ? 0.0f : static_cast<float>(elapsed) / static_cast<float>(fadeMs);
    }

} // namespace Render::SectionFade
