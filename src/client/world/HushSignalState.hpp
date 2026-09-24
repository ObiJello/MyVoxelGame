// File: src/client/world/HushSignalState.hpp
//
// The client's copy of the Hush's item signals (HushSignalS2CPacket): the
// tuning fork's ping outlines, the resonance arrow's bursts and the echo
// compass's target.
//
// Written on the network I/O thread (ClientConnection's raw handler, the
// HushStillnessS2C shape) and read on the main thread. A ping is a list, not
// a pair of numbers, so this state sits behind a mutex rather than in
// atomics; both sides hold it for a copy's length only.
//
// The main thread's half is DrawAndSpawn, called once per frame from the
// render loop in the main view: it queues the outlines as always-on-top
// gizmo cuboids (depth test off — the ping shows through walls, which is its
// point) and turns any pending ring or burst into HushMote particles.
#pragma once

#include "common/network/packets/game/HushSignalS2CPacket.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

namespace Client::HushSignalState {

    // Network I/O thread.
    void OnPacket(const Network::HushSignalS2CPacket& packet);

    // Main thread, once per frame, main view only. Queues this frame's ping
    // outlines on Render::Gizmos (the caller flushes) and spawns the pending
    // particle rings for `dimension` — signals for another dimension are
    // dropped (the player changed level under them).
    void DrawAndSpawn(Game::DimensionId dimension, float framebufferWidth);

    // The echo compass's target, if the server gave one for `dimension`.
    bool CompassTarget(Game::DimensionId dimension, double& x, double& z);

    // Forget everything (disconnect / world change).
    void Clear();

} // namespace Client::HushSignalState
