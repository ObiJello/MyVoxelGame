// File: src/common/core/Features.hpp
//
// ── Compile-time feature toggles ─────────────────────────────────────────────
//
// Central hub for "extra features" that can be COMPILED OUT entirely. When a
// flag is set to 0, every `#if ENABLE_<NAME> ... #endif` block in the codebase
// is stripped at preprocessing time — the disabled feature contributes
// ZERO compiled bytes, ZERO runtime cost, and ZERO symbols in the final
// binary. Disabling is equivalent to "the feature was never written."
//
// USE CASES
//   • Faster builds while iterating on the rest of the engine.
//   • Smaller / leaner shipping binary that doesn't carry a half-finished
//     experimental feature.
//   • Users / forks who want a stripped-down version of the game.
//   • Profiling: confirm a feature has no perf overhead by toggling it off.
//
// HOW TO TOGGLE
//   Edit the `#define ENABLE_<NAME> 1` line below — change to `0` to disable.
//   Rebuild. That's it. CMake intentionally has no opinion on these flags;
//   this header is the SINGLE SOURCE OF TRUTH so you don't have to think
//   about build configs every time you want to flip something.
//
//   (The `#ifndef` guards still exist as a safety hatch — if you ever DO
//   want a CMake override for CI matrix builds or similar, you can pass
//   `-DENABLE_<NAME>=0` and it'll win. Default behavior is "edit this file.")
//
// CONVENTIONS
//   • Every consumer of a feature MUST `#include "common/core/Features.hpp"`
//     and wrap its code in `#if ENABLE_<NAME>` / `#endif` blocks.
//   • Never reach behind a flag — no hardcoded portal-gun ItemIDs in
//     unrelated files, no `extern Portal::Foo` declarations outside guards.
//   • CMakeLists.txt lists feature .cpp files unconditionally. When a flag
//     is off, the .cpp's body strips to nothing and produces an empty .o —
//     no CMake editing required.
//   • If a feature needs runtime config (numbers, limits), expose it as a
//     SECOND macro alongside the enable flag (see PORTAL_RECURSION_DEPTH).

#pragma once

// ─── Portal Gun (Portal-game-style portals) ──────────────────────────────────
// Right-click-fired blue/orange portals on flat walls. Stepping through one
// teleports the player to the other with momentum carried (Portal-game
// "speedy thing goes in, speedy thing comes out"). Renderer uses a real
// stencil-buffer recursive scene render for the see-through view (Portal-game
// fidelity — full infinite-mirror when portals face each other).
//
// Cost when ENABLED: meaningful — adds a custom item, server-side portal
// registry per gun stack, network packets, and a multi-pass scene renderer
// that re-runs chunk meshing draws inside stencil-masked regions (up to
// PORTAL_RECURSION_DEPTH extra full scene passes per frame when looking
// down a portal-facing-portal chain).
#ifndef ENABLE_PORTAL_GUN
#define ENABLE_PORTAL_GUN 1
#endif

// Recursion budget for portal-in-portal rendering. Each level adds one full
// chunk-render pass per visible portal. Higher = more dramatic infinite
// mirror, lower = better frame rate when two portals face each other.
// 8-bit stencil buffer caps the hardware ceiling at 255; 5 is plenty for
// the visual effect to "feel infinite" without burning the GPU.
// Only meaningful when ENABLE_PORTAL_GUN.
#ifndef PORTAL_RECURSION_DEPTH
#define PORTAL_RECURSION_DEPTH 5
#endif
