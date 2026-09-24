// File: src/client/renderer/gui/debug/FrameProfiler.hpp
//
// The tree behind the F3+1 pie chart — this engine's stand-in for MC's
// ActiveProfiler (util/profiling): a push/pop path profiler whose sections
// carry vanilla's names ("tick", "frame", "render", "world", "level",
// "Main", "solidTerrain", …) so the pie reads like Minecraft's. MC only
// runs its ActiveProfiler while the chart is up (Minecraft.constructProfiler
// gates on showProfilerChart); this one does the same, and every call is a
// cheap early-out otherwise.
//
// This is ADDITIVE to the frame loop's own instrumentation: the
// PerformanceMetrics phase timers and the Tracy zones are untouched and keep
// their own names; the MC-named sections sit beside them.
#pragma once

#include "DebugCharts.hpp"

namespace Render::DebugScreen {

    namespace FrameProfiler {
        // Set once per frame from Overlay().ShowProfilerChart().
        void SetActive(bool active);
        bool IsActive();

        // Start the frame's tree ("root").
        void BeginFrame();
        // MC ProfilerFiller.push / popPush / pop.
        void Push(const char* name);
        void PopPush(const char* name);
        void Pop();
        // A child of the CURRENT section whose time is known from elsewhere
        // (the chunk renderer's per-pass timers): MC's tree gets these from
        // nested pushes; here the pass is one call, so its parts are added
        // as leaves after the fact.
        void AddChildTime(const char* name, double milliseconds);
        // Close the frame and hand the tree to the pie chart.
        void EndFrame(ProfileNode& outRoot);

        // RAII section. `DEBUG_PIE_ZONE("name")` next to a PROFILE_ZONE_N.
        struct Scope {
            explicit Scope(const char* name) { Push(name); }
            ~Scope() { Pop(); }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;
        };
    }

} // namespace Render::DebugScreen

#define DEBUG_PIE_CONCAT2(a, b) a##b
#define DEBUG_PIE_CONCAT(a, b) DEBUG_PIE_CONCAT2(a, b)
#define DEBUG_PIE_ZONE(name) ::Render::DebugScreen::FrameProfiler::Scope DEBUG_PIE_CONCAT(_debugPieZone, __LINE__)(name)
