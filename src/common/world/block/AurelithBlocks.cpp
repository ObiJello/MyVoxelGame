// File: src/common/world/block/AurelithBlocks.cpp
//
// See AurelithBlocks.hpp.
#include "common/world/block/AurelithBlocks.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/BlockState.hpp"

#include <cmath>

namespace Game {

    namespace {

        constexpr double kTau = 6.283185307179586;

        // ── resonance_engine ───────────────────────────────────────────────
        // The Heart still draws: motes spiral in toward the core from a few
        // blocks out and settle on it. A single block is landed on rarely,
        // so each landing sends a small flock.
        void EngineAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                               BlockState /*state*/, JavaRandom& random) {
            for (int i = 0; i < 6; ++i) {
                const double angle = random.NextDouble() * kTau;
                const double r = 2.0 + random.NextDouble() * 3.5;
                const double dx = std::cos(angle) * r;
                const double dz = std::sin(angle) * r;
                const double dy = -0.5 + random.NextDouble() * 4.0;
                // Inward and a little tangential, so the flock swirls.
                const double vx = -dx * 0.025 - dz * 0.01;
                const double vz = -dz * 0.025 + dx * 0.01;
                level.AddParticle(ParticleKind::HushMote,
                                  pos.x + 0.5 + dx, pos.y + 0.5 + dy, pos.z + 0.5 + dz,
                                  vx, -dy * 0.015, vz);
            }
        }

        // ── voice_beacon ───────────────────────────────────────────────────
        // Motes ride the beam up out of the lens.
        void BeaconAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                               BlockState /*state*/, JavaRandom& random) {
            for (int i = 0; i < 4; ++i) {
                level.AddParticle(ParticleKind::HushMote,
                                  pos.x + 0.35 + random.NextDouble() * 0.3,
                                  pos.y + 1.1 + random.NextDouble() * 6.0,
                                  pos.z + 0.35 + random.NextDouble() * 0.3,
                                  0.0, 0.08 + random.NextDouble() * 0.06, 0.0);
            }
        }

        // ── choir_lamp ─────────────────────────────────────────────────────
        // The streetlights shed the odd glint, the way the lighthouse lamp
        // does, so lumen moths and motes gather at the lamps.
        void LampAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                             BlockState /*state*/, JavaRandom& random) {
            if (random.NextInt(3) != 0) return;
            const double angle = random.NextDouble() * kTau;
            const double r = 0.6 + random.NextDouble() * 0.5;
            level.AddParticle(ParticleKind::HushMote,
                              pos.x + 0.5 + std::cos(angle) * r,
                              pos.y + 0.2 + random.NextDouble() * 0.6,
                              pos.z + 0.5 + std::sin(angle) * r,
                              0.0, 0.01, 0.0);
        }

    } // namespace

    void BlockRegistry_RegisterAurelithBlocks(std::array<Block, BlockRegistry::Size>& blocks) {
        // Full-bright: the city's light. Render-only (the mesher tags the
        // faces and the terrain shaders skip the night dim); nothing here
        // lights its neighbours.
        for (BlockID id : { BlockID::StaveStone, BlockID::CyanLumenPanel,
                            BlockID::VioletLumenPanel, BlockID::AmberLumenPanel,
                            BlockID::LumenStrip, BlockID::CrystalConduit,
                            BlockID::ChoirLamp, BlockID::ResonantWater,
                            BlockID::ResonanceEngine, BlockID::VoiceBeacon,
                            // The flickering windows: their dark frames are
                            // painted dark, so full-bright shows them unlit.
                            BlockID::GutteringAmberWindow, BlockID::WakingAmberWindow,
                            BlockID::RestlessAmberWindow, BlockID::GutteringCyanWindow,
                            BlockID::WakingCyanWindow, BlockID::GutteringVioletWindow,
                            BlockID::WakingVioletWindow }) {
            blocks[static_cast<size_t>(id)].emissive = true;
        }

        // resonant_water sheds no particles: the river is its colour and its
        // glow, nothing rises off it.
        blocks[static_cast<size_t>(BlockID::ResonanceEngine)].animateTick = &EngineAnimateTick;
        blocks[static_cast<size_t>(BlockID::VoiceBeacon)].animateTick     = &BeaconAnimateTick;
        blocks[static_cast<size_t>(BlockID::ChoirLamp)].animateTick       = &LampAnimateTick;
    }

} // namespace Game
