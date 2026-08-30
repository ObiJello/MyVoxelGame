// File: src/client/world/ClientAnimateTick.hpp
//
// MC ClientLevel.animateTick — the client's ambient-particle sweep.
//
// This engine had NO equivalent, which is why it has no ambient block
// particles at all: no torch flames, no portal shimmer, no lava pops, and no
// dust trickling from unsupported sand. Every one of those comes from MC's
// `animateTick`, and none of them is a server concept — the server never knows
// they happened.
//
// The sweep is deliberately dumb: 667 iterations, each sampling TWO random
// positions around the camera (one within 16 blocks, one within 32) and running
// whatever block it lands on. That is 1334 block lookups per client tick, which
// sounds wasteful and is exactly what vanilla does — the tight box is sampled
// ~8x more densely than the loose one, so nearby blocks animate reliably while
// distant ones only flicker.
#pragma once

#include <glm/glm.hpp>

namespace Game {
    struct IBlockAccess;
    struct EntityLevel;
    class  JavaRandom;
}

namespace Client {

    // MC ClientLevel.animateTick(x, y, z), called once per client tick with the
    // camera's block position.
    //
    // `blocks` is the client's chunk view and `particleSink` is where the
    // spawned particles go — both are already available at the call site, and
    // taking them explicitly keeps this testable without a live level.
    void AnimateTick(const glm::ivec3& cameraBlockPos,
                     const Game::IBlockAccess& blocks,
                     Game::EntityLevel& particleSink,
                     Game::JavaRandom& random);

} // namespace Client
