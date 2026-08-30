// File: src/client/world/ClientAnimateTick.cpp
#include "client/world/ClientAnimateTick.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/World.hpp"

namespace Client {

    namespace {

        // MC ClientLevel.animateTick's loop count and its two radii. 667 x 2
        // samples per tick; the 16-block box is sampled at the same rate as the
        // 32-block one, so a cell nearby is picked roughly eight times as often
        // as one far away.
        constexpr int kIterations = 667;
        constexpr int kNearRadius = 16;
        constexpr int kFarRadius  = 32;

        // MC doAnimateTick(xt, yt, zt, r, ...): one sample, uniform in a cube
        // of side 2r centred on the camera. Note the `nextInt(r) - nextInt(r)`
        // form — that is a TRIANGULAR distribution, not uniform, so samples
        // cluster toward the camera even within one radius.
        void DoAnimateTick(const glm::ivec3& centre, int radius,
                           const Game::IBlockAccess& blocks,
                           Game::EntityLevel& sink,
                           Game::JavaRandom& random) {
            const glm::ivec3 pos(
                centre.x + random.NextInt(radius) - random.NextInt(radius),
                centre.y + random.NextInt(radius) - random.NextInt(radius),
                centre.z + random.NextInt(radius) - random.NextInt(radius));

            if (pos.y < Game::World::MIN_Y || pos.y > Game::World::MAX_Y) return;

            const Game::BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
            const Game::BlockID    id    = state.Block();
            if (id == Game::BlockID::Air) return;   // the overwhelmingly common case

            const Game::Block& def = Game::BlockRegistry::Get(id);
            if (!def.animateTick) return;
            def.animateTick(sink, pos, state, random);
        }

    } // namespace

    void AnimateTick(const glm::ivec3& cameraBlockPos,
                     const Game::IBlockAccess& blocks,
                     Game::EntityLevel& particleSink,
                     Game::JavaRandom& random) {
        for (int i = 0; i < kIterations; ++i) {
            DoAnimateTick(cameraBlockPos, kNearRadius, blocks, particleSink, random);
            DoAnimateTick(cameraBlockPos, kFarRadius,  blocks, particleSink, random);
        }
    }

} // namespace Client
