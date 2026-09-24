// File: src/common/sound/PlayerMovementSounds.hpp
//
// The sounds a PLAYER's own movement makes — MC Entity.move's
// applyMovementEmissionAndPlaySound (footsteps, the amethyst chime, swimming),
// Entity.updateFluidInteraction's water-entry splash (doWaterSplashEffect)
// and LivingEntity.causeFallDamage's landing (fall damage sound + the block
// fall sound), with Player's overrides (getMovementEmission, playStepSound's
// in-water and combination rules, getSwimSound / getSwimSplashSound /
// getFallSounds).
//
// Why a shared helper rather than Entity code: a player is not a
// Game::Entity on the client, and player movement is CLIENT-authoritative
// here. MC runs Entity.move for a player on both sides — the LocalPlayer
// plays its own steps locally (LocalPlayer.playSound → playLocalSound) and
// the server's replay of the reported move plays them for everyone else
// (Player.playSound → level.playSound(this, ...), except the mover). This
// helper is that one piece of logic; the client feeds it the local physics
// each tick and plays the result locally, the server feeds it each move
// packet and broadcasts the result with the player excepted.
//
// Everything here is per TICK, in MC's units (blocks per tick).
#pragma once

#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace Game {

    struct IBlockAccess;
    class JavaRandom;

    struct PlayerMovementSound {
        const char* event;   // a SoundEvents id
        float       volume;
        float       pitch;
    };

    class PlayerMovementSounds {
    public:
        struct Input {
            glm::dvec3 previousPosition{0.0};   // feet, last tick
            glm::dvec3 position{0.0};           // feet, now
            bool onGround = false;
            bool crouching = false;             // Player.isDiscrete (sneaking)
            bool flying = false;                // abilities.flying
            bool noPhysics = false;             // spectator / noclip: no emission at all
            bool inWater = false;               // Entity.isInWater
            bool swimming = false;              // Entity.isSwimming (sprint-swimming)
        };

        // One tick of movement. Appends what to play.
        void Tick(const IBlockAccess& blocks, const Input& input, JavaRandom& random,
                  std::vector<PlayerMovementSound>& out);

        // MC LivingEntity.causeFallDamage's sound half, for a landing of
        // `fallDistance` blocks at `feet`: the fall damage sound (PLAYER_SMALL
        // _FALL / PLAYER_BIG_FALL by damage) and the landed-on block's fall
        // sound — only when the fall hurts (damage > 0), as in MC.
        // `safeFallDistance` is MC's SAFE_FALL_DISTANCE attribute (3 + jump
        // boost), `scale` the body scale the fall is measured against.
        static void Landing(const IBlockAccess& blocks, const glm::dvec3& feet, float fallDistance,
                            float scale, float safeFallDistance, std::vector<PlayerMovementSound>& out);

        // Teleports, respawns, dimension changes: start the step counters over.
        void Reset();

    private:
        float m_moveDist = 0.0f;
        float m_nextStep = 1.0f;                   // MC Entity.nextStep's initialiser
        bool  m_wasTouchingWater = false;
        bool  m_firstTick = true;
        int   m_tickCount = 0;
        int   m_lastCrystalSoundPlayTick = 0;
        float m_crystalSoundIntensity = 0.0f;
    };

} // namespace Game
