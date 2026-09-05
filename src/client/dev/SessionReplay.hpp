// File: src/client/dev/SessionReplay.hpp
//
// Pose recording and replay — the profiling harness's "same scene every
// run" tool.
//
// A recording is the local player's pose, sampled once per frame: feet
// position, dimension, camera yaw/pitch and the physics flags that shape
// the view (sneak = eye height, sprint speed / flight = FOV, scale). Replay
// drives the player along that path BY TIME, interpolating between the two
// samples around the replay clock, so the camera is at the same place at
// the same second whatever the frame rate — a 60 fps GL run and a 200 fps
// Vulkan run of the same recording render the same sequence of views and
// their `[Harness]` fps lines can be compared second by second.
//
// What replay does NOT reproduce: input. The player's physics step is
// skipped for the replayed frames (the pose is authoritative), so nothing
// is broken or placed, mobs are not hit, and the hotbar is whatever it
// was. Chunk loading, mesh scheduling, entity ticking and the server all
// see the player move exactly as they did — the pose still goes out in
// PlayerMoveC2S like any other movement.
//
// Portals. An immersive-portal crossing is decided per frame on the eye's
// path (ImmersivePortalTraveler::Check), and the sample after a crossing
// is in far-side coordinates — possibly another dimension. Replaying
// that as a straight interpolation would fling the eye across the world
// and never through the surface. So a sample flagged PortalCrossing marks
// an interval the replay does not interpolate: it extrapolates the
// pre-crossing motion until the traveler commits a crossing of its own
// (PoseReplayer::OnCrossing), then snaps to the recorded far-side path.
// The dimension switch, the server notification and the render-side level
// rebinding all happen through the real crossing code, unchanged.
// Server-initiated jumps (a /tp, a respawn) are flagged Teleport and
// applied as a snap when the recorded dimension matches the live one;
// a cross-dimension teleport cannot be reproduced and ends the replay.
//
// Files live in <obeycraft>/recordings/<name>.rec, plain text, one sample
// per line (see Recording::Save), so a script can read or trim them.
#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Game { class ClientPlayer; }
namespace Render { class Camera; }

namespace Client::Dev {

    struct PoseSample {
        // How the frame that produced this sample got here.
        enum class Arrival : uint8_t {
            Move,            // continuous motion from the previous sample
            PortalCrossing,  // an immersive/gun portal crossing committed this frame
            Teleport,        // a discontinuity that was not a crossing (tp, respawn)
        };
        double     t     = 0.0;   // seconds since the recording started
        int8_t     dim   = 0;     // Game::DimensionToRaw of the active level
        glm::dvec3 pos{0.0};      // feet (PlayerPhysics::position)
        float      yaw   = 0.0f;  // camera yaw/pitch, degrees (MC convention)
        float      pitch = 0.0f;
        float      scale = 1.0f;  // PlayerPhysics::scale (portal scaling)
        float      speed = 0.0f;  // PlayerPhysics::currentSpeed (drives the FOV)
        bool       sneak = false;
        bool       sprint = false;
        bool       fly   = false;
        bool       onGround = false;
        Arrival    arrival = Arrival::Move;
    };

    struct Recording {
        std::string world;                 // display name of the world it was made in
        std::string recordedAt;            // local time, informational
        std::vector<PoseSample> samples;

        double Duration() const { return samples.empty() ? 0.0 : samples.back().t; }

        // <obeycraft>/recordings — created on demand by Save.
        static std::string Directory();
        // Directory()/<name>.rec ; a name that already ends in .rec or
        // contains a path separator is used as given.
        static std::string PathFor(const std::string& name);

        bool Load(const std::string& path, std::string& error);
        bool Save(const std::string& path, std::string& error) const;
    };

    // Samples the player once per frame into a Recording and writes it out
    // on Stop (or destruction). Start it once the level has loaded — the
    // first Sample() call is t=0.
    class PoseRecorder {
    public:
        ~PoseRecorder();

        bool Start(const std::string& name, const std::string& world);
        void Sample(const Game::ClientPlayer& player, const Render::Camera& camera,
                    int8_t dim, bool crossedThisFrame);
        // Writes the file. Safe to call when not recording.
        void Stop();

        bool               Active() const { return m_active; }
        const std::string& Name()   const { return m_name; }
        double             Elapsed() const;
        size_t             SampleCount() const { return m_recording.samples.size(); }

    private:
        bool        m_active = false;
        std::string m_name;
        std::string m_path;
        Recording   m_recording;
        double      m_clock       = 0.0;   // set from the frame clock at Sample
        bool        m_haveStart   = false;
        long long   m_startNs     = 0;
        double      m_nextLogAt   = 5.0;
    };

    // Drives the player along a Recording. Call Apply() once per frame IN
    // PLACE OF the player's physics step while Active(); it handles the
    // initial snap to the first sample, the hold that lets the world stream
    // in around it, the timed playback and the portal handshake.
    class PoseReplayer {
    public:
        enum class Stage : uint8_t { Idle, Hold, Playing, Done };

        // Loads Recording::PathFor(name). `holdSeconds` is how long the
        // player is parked on the first sample before the clock starts —
        // the same for every run, so the chunk streaming state at t=0 is
        // too. Logs and returns false when the file cannot be read.
        bool Load(const std::string& name, double holdSeconds);
        void Stop();   // abandons a replay in progress (Done, no snap)

        bool  Active()   const { return m_stage == Stage::Hold || m_stage == Stage::Playing; }
        bool  Finished() const { return m_stage == Stage::Done; }
        Stage GetStage() const { return m_stage; }

        // One frame of playback. `activeDim` is DimensionToRaw of the
        // client's active level. Writes the pose onto the player and the
        // camera; sets `velocity` to the finite difference so the portal
        // traveler and the move packet see a moving player.
        void Apply(Game::ClientPlayer& player, Render::Camera& camera, float dt, int8_t activeDim);

        // The frame's crossing code committed a portal crossing (after
        // Apply, same frame). Lets a PortalCrossing interval resolve.
        void OnCrossing();

        // Replay clock in seconds: negative during the hold, 0 at the first
        // recorded sample. For log lines and the Tracy plot.
        double Time() const;
        double Duration() const { return m_recording.Duration(); }
        size_t Cursor()   const { return m_cursor; }
        const std::string& Name() const { return m_name; }

        // One "[Replay] t=... " line; the harness calls it once a second.
        void LogProgress() const;

    private:
        void Finish(const char* why);
        void Snap(Game::ClientPlayer& player, Render::Camera& camera, const PoseSample& s) const;
        void Write(Game::ClientPlayer& player, Render::Camera& camera, const PoseSample& s,
                   const glm::dvec3& pos, float yaw, float pitch, float dt) const;

        Stage       m_stage = Stage::Idle;
        std::string m_name;
        Recording   m_recording;
        double      m_holdSeconds = 0.0;
        double      m_holdElapsed = 0.0;
        double      m_clock       = 0.0;   // seconds into the recording
        size_t      m_cursor      = 0;     // last sample with t <= clock
        // Portal handshake for the interval cursor → cursor+1.
        bool        m_crossingSeen = false;
        double      m_waitLoggedAt = -1.0;
    };

} // namespace Client::Dev
