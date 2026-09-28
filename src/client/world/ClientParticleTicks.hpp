// File: src/client/world/ClientParticleTicks.hpp
//
// The client-side particle sources MC runs from ClientLevel outside the
// blocks' own animateTick:
//
//   FluidDrip          ClientLevel.doAnimateTick → trySpawnDripParticles:
//                      water and lava dripping through the block under a
//                      fluid (a cave ceiling under a lake).
//   AmbientParticles   doAnimateTick's AMBIENT_PARTICLES step — the biome's
//                      ambient motes (basalt deltas' white ash, the soul sand
//                      valley's ash, the crimson and warped spores), read from
//                      the biome JSONs' "minecraft:visual/ambient_particles".
//   TickWeather        ClientLevel.tickWeatherEffects' particles: rain
//                      splashes on the ground around the camera (smoke on lava,
//                      magma and lit campfires).
//   TickBlockEntities  the client block-entity particle tickers:
//                      CampfireBlockEntity.particleTick (the smoke column and
//                      the smoking food) for every loaded campfire.
//   TickPlayers        Entity.baseTick's sprint dust and water-entry splash
//                      for the local player and the remote players.
//
// Main thread, against the ACTIVE level (the bridge the particles queue on).
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/fluid/FluidState.hpp"

#include <glm/glm.hpp>

namespace Game {
    struct IBlockAccess;
    class  JavaRandom;
    class  ClientPlayer;
}

namespace Client {

    class ClientLevelBridge;
    class ClientChunkManager;

    namespace ParticleTicks {

        void FluidDrip(const glm::ivec3& pos, Game::BlockState state, const Game::FluidState& fluid,
                       const Game::IBlockAccess& blocks, ClientLevelBridge& sink, Game::JavaRandom& random);

        void AmbientParticles(const glm::ivec3& pos, Game::BlockState state, ClientLevelBridge& sink,
                              Game::JavaRandom& random);

        void TickWeather(const glm::dvec3& camera, const Game::IBlockAccess& blocks, ClientLevelBridge& sink);

        void TickBlockEntities(ClientChunkManager& chunks, const Game::IBlockAccess& blocks, ClientLevelBridge& sink);

        // Entity.baseTick's particles for the players this client draws
        // (the local one and every remote copy): the sprint dust
        // (spawnSprintParticle) and the water-entry ring of bubbles and
        // splashes (doWaterSplashEffect — its sound is PlayerMovementSounds').
        // Mobs run theirs in their own Entity::BaseTick.
        void TickPlayers(const Game::ClientPlayer& local, const Game::IBlockAccess& blocks, ClientLevelBridge& sink);

        // The local player's server-side /invisible flag (SelfParticleStateS2C).
        // Engine rule: an invisible player — this flag or the INVISIBILITY
        // effect — kicks up no sprint dust or splash.
        void SetLocalPlayerInvisible(bool invisible);
        bool IsLocalPlayerInvisible();

    } // namespace ParticleTicks

} // namespace Client
