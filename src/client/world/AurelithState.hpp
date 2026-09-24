// File: src/client/world/AurelithState.hpp
//
// The client's copy of the Aurelith cities near the player (AurelithS2C,
// docs/the-hush.md "Reawakening the Heart"), and the one place every client
// system reads the quest's animation from: the Heart's rings, the gate beams
// bending into one pillar, the light and motes, the city's hum and music.
//
// Each record is (Heart, rotation, state, stageStartTick, awakenedTick). The
// helpers below turn a record and the level's game time (EnvironmentState::
// GameTimeF on the render side, the client level's game time on the tick
// side) into the numbers the consumers need, on Game::Aurelith's timeline —
// so the renderer, the particles and the sound can never disagree about
// "now". They are pure functions: no state, no side effects.
//
// Threads: OnPacket and Clear run on the network I/O thread; everything else
// is read from the main / render thread. Records sit behind a mutex; readers
// get copies.
#pragma once

#include "common/network/packets/game/AurelithS2CPacket.hpp"
#include "common/world/level/AurelithQuest.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Client::AurelithState {

    struct City {
        Game::DimensionId dimension = Game::DimensionId::Hush;
        glm::ivec3 heart{0};
        int        rotation = -1;              // -1: unknown (a legacy city)
        Game::Aurelith::CityState state = Game::Aurelith::CityState::Dormant;
        int64_t    stageStartTick = 0;         // game tick the current state began
        int64_t    awakenedTick = 0;           // 0: never awakened

        bool KnowsRotation() const { return rotation >= 0; }
    };

    // ── The record (network thread in, any thread out) ──────────────────
    void OnPacket(const Network::AurelithS2CPacket& packet);
    void Clear();

    // Main thread, once per frame (beside HushSignalState::DrawAndSpawn):
    // turn the server's pending Bursts for `dimension` into particles.
    // Bursts for another dimension are dropped.
    void SpawnPendingBursts(Game::DimensionId dimension);

    // Main thread, once per frame: the living light of the awakened cities
    // near `camera` — motes drifting up out of the Heart (a steady stream
    // once awake, a torrent while the Chord is sung, dark motes sinking
    // round the dais while the Undersong answers). The river itself sheds no
    // particles. Spawns once per new game tick (`gameTick`), so the rate
    // does not follow the frame rate.
    void TickParticles(Game::DimensionId dimension, const glm::dvec3& camera, int64_t gameTick);

    std::vector<City> Snapshot(Game::DimensionId dimension);
    // The city whose Heart is at `heart` (exact), if known.
    std::optional<City> CityAt(Game::DimensionId dimension, const glm::ivec3& heart);
    // The nearest city (horizontal distance to the Heart) within `maxDistance`.
    std::optional<City> Nearest(Game::DimensionId dimension, const glm::dvec3& pos, double maxDistance);
    // The city whose walls `pos` is inside (Game::Aurelith::InsideWalls).
    std::optional<City> InsideWalls(Game::DimensionId dimension, const glm::dvec3& pos);

    // ── The animation, from (record, game time) ─────────────────────────
    // Ticks since the current stage began (0 before it).
    double StageTicks(const City& c, double ticks);

    // How awake the city's light is, 0 (dormant: the lights dim) .. 1
    // (full). Rises with the light wave in the Awakening stage, holds 1 while
    // Contested and Awakened. Per position: `WaveLitAt` says whether the wave
    // has reached a point yet.
    double LightLevel(const City& c, double ticks);
    bool   WaveLitAt(const City& c, double ticks, const glm::dvec3& pos);
    double WaveRadius(const City& c, double ticks);

    // The rings' pace as a multiple of their dormant pace: 1 dormant, a
    // smooth spin-up in the Awakening stage, faltering under the Undersong,
    // and a steady 3x once awakened (the Chord held again).
    double RingPace(const City& c, double ticks);
    // A 0..1 "the Chord is being sung" swell for glows and hums: 0 dormant,
    // rising with the spin-up, 1 at full voice.
    double Voice(const City& c, double ticks);
    // The Undersong: 0 .. 1 while it answers (the soured note — the rings
    // stutter, the pillar darkens toward violet); peaks while Contested and
    // fades out over the resolution.
    double Sourness(const City& c, double ticks);
    // How far the four gate beams have bent together into the pillar over
    // the Heart: 0 straight up .. 1 converged. Eased.
    double BeamConvergence(const City& c, double ticks);
    // The resolution swell after the Unsung falls: 1 at the moment of
    // victory falling to 0 over Game::Aurelith::kResolveTicks.
    double Resolution(const City& c, double ticks);

    // The Voice a voice beacon at `beaconPos` sings in this city: its gate's
    // design direction (the beacon's facing is turned by the template
    // rotation, so the world facing names the wrong voice in a rotated city).
    // -1 when the city's rotation is unknown.
    int BeaconVoice(const City& c, const glm::ivec3& beaconPos);

} // namespace Client::AurelithState
