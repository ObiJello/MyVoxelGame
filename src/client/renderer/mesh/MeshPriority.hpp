// File: src/client/renderer/mesh/MeshPriority.hpp
//
// The client's mesh ORDER, portal-aware. One key serves the scheduler's
// candidate sort (ClientChunkManager::ScheduleMeshBuildsWithSnapshots) and
// the compile queue's poll against the live camera
// (ClientWorkerPool::PollNearestLocked), so a section's rank is the same at
// admission, at every re-mesh, and for sections that arrive later.
//
// Per level, a Field holds:
//   • direct — the viewer stands in this level; the plain distance from its
//     camera counts (the metric every level used before: squared, with Y
//     attenuated by 0.1 — see PollNearestLocked);
//   • routes — every portal of the viewer's level that leads here and that
//     the viewer could look through (Game::PortalRoute): walk to the
//     surface, then the far leg from where the view comes out.
// A section's key is the minimum over all of them (squared, so it compares
// with the plain metric). A level with neither — a far level seen only
// through a portal of a portal — keeps its own camera's plain metric.
//
// Built once a frame on the main thread (Render::UpdateMeshPortalRoutes,
// from a per-level portal candidate list cached on the viewer's section and
// the portal store's revision) and published to the worker pool.
#pragma once

#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Render::MeshPriority {

    struct Route {
        glm::vec3 farPoint{0.0f};   // where the view comes out, in this level
        float     entryCost = 0.0f; // blocks from the viewer's eye to the surface
        float     invScale  = 1.0f; // far-side distances count this much
        uint32_t  portalId  = 0;    // for the log

        bool operator==(const Route& o) const {
            return farPoint == o.farPoint && entryCost == o.entryCost &&
                   invScale == o.invScale && portalId == o.portalId;
        }
    };

    // Fixed capacity: the field is copied under the worker pool's lock on
    // every poll, so it must be trivially copyable. Routes to the same far
    // area merge (Add); more distinct areas than this into one level keep
    // the nearest (by entry cost).
    inline constexpr int kMaxRoutes = 16;
    // Two routes whose far points are this close lead to the same area.
    inline constexpr float kSameAreaBlocks = 16.0f;

    struct Field {
        bool direct = true;
        int  count  = 0;
        std::array<Route, kMaxRoutes> routes{};

        void Clear(bool isDirect) { direct = isDirect; count = 0; }
        void Add(const Route& r) {
            // One route per far area (a bi-faced surface's two records, a
            // portal and its flipped twin): the cheapest walk there.
            for (int i = 0; i < count; ++i) {
                const glm::vec3 d = routes[i].farPoint - r.farPoint;
                if (glm::dot(d, d) <= kSameAreaBlocks * kSameAreaBlocks) {
                    if (r.entryCost < routes[i].entryCost) routes[i] = r;
                    return;
                }
            }
            if (count < kMaxRoutes) { routes[count++] = r; return; }
            // Full: replace the farthest entry if this one is nearer.
            int worst = 0;
            for (int i = 1; i < count; ++i) if (routes[i].entryCost > routes[worst].entryCost) worst = i;
            if (r.entryCost < routes[worst].entryCost) routes[worst] = r;
        }
        bool operator==(const Field& o) const {
            if (direct != o.direct || count != o.count) return false;
            for (int i = 0; i < count; ++i) if (!(routes[i] == o.routes[i])) return false;
            return true;
        }
        bool operator!=(const Field& o) const { return !(*this == o); }
    };

    using Fields = std::array<Field, Game::kDimensionCount>;

    inline glm::vec3 SectionCentre(Game::Math::ChunkPos chunkPos, int sectionY) {
        return glm::vec3(chunkPos.x * 16.0f + 8.0f,
                         -64.0f + sectionY * 16.0f + 8.0f,
                         chunkPos.z * 16.0f + 8.0f);
    }

    // The engine's mesh-order metric: squared, Y attenuated by 0.1 (0.01
    // squared) — see PollNearestLocked for why.
    inline float MetricSq(const glm::vec3& d) {
        return d.x * d.x + d.z * d.z + d.y * d.y * 0.01f;
    }

    // Plain metric from the camera (what the key was before portals).
    inline float DirectDistSq(const glm::vec3& camera, Game::Math::ChunkPos chunkPos, int sectionY) {
        return MetricSq(SectionCentre(chunkPos, sectionY) - camera);
    }

    // The priority key. `outRoute` (optional): the winning route's index,
    // -1 when the direct metric won.
    inline float EffectiveDistSq(const Field& field, const glm::vec3& camera,
                                 Game::Math::ChunkPos chunkPos, int sectionY,
                                 int* outRoute = nullptr) {
        const glm::vec3 c = SectionCentre(chunkPos, sectionY);
        float best = (field.direct || field.count == 0) ? MetricSq(c - camera)
                                                        : std::numeric_limits<float>::max();
        int bestRoute = -1;
        for (int i = 0; i < field.count; ++i) {
            const Route& r = field.routes[i];
            const float d = r.entryCost + std::sqrt(MetricSq(c - r.farPoint)) * r.invScale;
            const float dSq = d * d;
            if (dSq < best) { best = dSq; bestRoute = i; }
        }
        if (outRoute) *outRoute = bestRoute;
        return best;
    }

    // Nearest a COLUMN is through any route, in blocks, horizontally (the
    // scheduler's "is this column near the viewer" gate); +inf with none.
    inline float ColumnRouteDistance(const Field& field, Game::Math::ChunkPos chunkPos) {
        float best = std::numeric_limits<float>::infinity();
        const float cx = chunkPos.x * 16.0f + 8.0f;
        const float cz = chunkPos.z * 16.0f + 8.0f;
        for (int i = 0; i < field.count; ++i) {
            const Route& r = field.routes[i];
            const float dx = cx - r.farPoint.x;
            const float dz = cz - r.farPoint.z;
            best = std::min(best, r.entryCost + std::sqrt(dx * dx + dz * dz) * r.invScale);
        }
        return best;
    }

    // ── One-shot [MeshOrder] log ─────────────────────────────────────────
    // Armed when a level load starts (LevelLoadTracker::StartClientLoad);
    // each level's scheduler logs its first pass after that in which a
    // portal route won a candidate, within kOrderLogWindow.
    inline constexpr auto kOrderLogWindow = std::chrono::seconds(15);
    inline std::atomic<uint32_t>& OrderLogMask() { static std::atomic<uint32_t> mask{0}; return mask; }
    inline std::atomic<int64_t>&  OrderLogArmedAtMs() { static std::atomic<int64_t> at{0}; return at; }
    inline int64_t SteadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    inline void ArmOrderLog() {
        OrderLogArmedAtMs().store(SteadyNowMs(), std::memory_order_relaxed);
        OrderLogMask().store((1u << Game::kDimensionCount) - 1u, std::memory_order_release);
    }
    inline bool OrderLogArmed(Game::DimensionId dimension) {
        const uint32_t bit = 1u << Game::DimensionSlot(dimension);
        if ((OrderLogMask().load(std::memory_order_acquire) & bit) == 0) return false;
        const int64_t elapsed = SteadyNowMs() - OrderLogArmedAtMs().load(std::memory_order_relaxed);
        if (elapsed > std::chrono::duration_cast<std::chrono::milliseconds>(kOrderLogWindow).count()) {
            OrderLogMask().fetch_and(~bit, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }
    inline void DisarmOrderLog(Game::DimensionId dimension) {
        OrderLogMask().fetch_and(~(1u << Game::DimensionSlot(dimension)), std::memory_order_acq_rel);
    }

} // namespace Render::MeshPriority
