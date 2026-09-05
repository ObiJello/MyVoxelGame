// File: src/client/dev/SessionReplay.cpp
#include "SessionReplay.hpp"

#include "client/entity/Player.hpp"
#include "client/renderer/core/Camera.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "platform/GameDirectory.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Client::Dev {

    namespace {
        constexpr const char* kMagic = "# obeycraft pose recording v1";
        // A move of more than this between two consecutive frames that was
        // not a portal crossing is a teleport, not motion: at 20 fps the
        // fastest thing the player does (creative sprint-fly, ~40 b/s)
        // covers 2 blocks a frame.
        constexpr double kTeleportJumpBlocks = 8.0;
        // How long past a PortalCrossing sample's time the replay keeps
        // extrapolating before it gives up on the crossing reproducing.
        constexpr double kCrossingGraceSec = 0.5;
        // Same for a Teleport sample whose dimension the client is not in.
        constexpr double kTeleportGraceSec = 2.0;

        long long NowNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        float LerpYaw(float a, float b, double f) {
            // Shortest arc, so a 350 → 10 turn goes through 0, not 180.
            float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
            return a + d * static_cast<float>(f);
        }

        char ArrivalCode(PoseSample::Arrival a) {
            switch (a) {
                case PoseSample::Arrival::PortalCrossing: return 'c';
                case PoseSample::Arrival::Teleport:       return 't';
                default:                                  return 'm';
            }
        }

        PoseSample::Arrival ArrivalFromCode(char c) {
            switch (c) {
                case 'c': return PoseSample::Arrival::PortalCrossing;
                case 't': return PoseSample::Arrival::Teleport;
                default:  return PoseSample::Arrival::Move;
            }
        }
    }

    // ── Recording ─────────────────────────────────────────────────────

    std::string Recording::Directory() {
        return Platform::GameDirectory::GetDefaultGameDirectory() + "/recordings";
    }

    std::string Recording::PathFor(const std::string& name) {
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            (name.size() > 4 && name.compare(name.size() - 4, 4, ".rec") == 0)) {
            return name;
        }
        return Directory() + "/" + name + ".rec";
    }

    bool Recording::Save(const std::string& path, std::string& error) const {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        if (!out) { error = "cannot open " + path + " for writing"; return false; }
        out << kMagic << '\n'
            << "world " << world << '\n'
            << "recorded " << recordedAt << '\n'
            << "samples " << samples.size() << '\n'
            << "duration " << Duration() << '\n'
            << "# t dim x y z yaw pitch scale speed flags(s=sneak r=sprint f=fly g=ground) arrival(m=move c=portal t=teleport)\n";
        char line[256];
        for (const PoseSample& s : samples) {
            char flags[6];
            int n = 0;
            if (s.sneak)  flags[n++] = 's';
            if (s.sprint) flags[n++] = 'r';
            if (s.fly)    flags[n++] = 'f';
            if (s.onGround) flags[n++] = 'g';
            if (n == 0)   flags[n++] = '-';
            flags[n] = '\0';
            std::snprintf(line, sizeof line, "%.4f %d %.4f %.4f %.4f %.3f %.3f %.4f %.3f %s %c\n",
                          s.t, static_cast<int>(s.dim), s.pos.x, s.pos.y, s.pos.z,
                          s.yaw, s.pitch, s.scale, s.speed, flags, ArrivalCode(s.arrival));
            out << line;
        }
        if (!out) { error = "write failed for " + path; return false; }
        return true;
    }

    bool Recording::Load(const std::string& path, std::string& error) {
        std::ifstream in(path);
        if (!in) { error = "cannot open " + path; return false; }
        std::string line;
        if (!std::getline(in, line) || line != kMagic) {
            error = path + " is not a pose recording (bad header)";
            return false;
        }
        samples.clear();
        world.clear();
        recordedAt.clear();
        int lineNo = 1;
        while (std::getline(in, line)) {
            ++lineNo;
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("world ", 0) == 0)    { world = line.substr(6); continue; }
            if (line.rfind("recorded ", 0) == 0) { recordedAt = line.substr(9); continue; }
            if (line.rfind("samples ", 0) == 0 || line.rfind("duration ", 0) == 0) continue;
            PoseSample s;
            int dim = 0;
            char flags[16] = {0};
            char arrival = 'm';
            const int got = std::sscanf(line.c_str(), "%lf %d %lf %lf %lf %f %f %f %f %15s %c",
                                        &s.t, &dim, &s.pos.x, &s.pos.y, &s.pos.z,
                                        &s.yaw, &s.pitch, &s.scale, &s.speed, flags, &arrival);
            if (got < 11) {
                error = path + ":" + std::to_string(lineNo) + ": malformed sample";
                return false;
            }
            s.dim      = static_cast<int8_t>(dim);
            s.sneak    = std::strchr(flags, 's') != nullptr;
            s.sprint   = std::strchr(flags, 'r') != nullptr;
            s.fly      = std::strchr(flags, 'f') != nullptr;
            s.onGround = std::strchr(flags, 'g') != nullptr;
            s.arrival  = ArrivalFromCode(arrival);
            if (!samples.empty() && s.t < samples.back().t) {
                error = path + ":" + std::to_string(lineNo) + ": time goes backwards";
                return false;
            }
            samples.push_back(s);
        }
        if (samples.empty()) { error = path + " has no samples"; return false; }
        return true;
    }

    // ── PoseRecorder ──────────────────────────────────────────────────

    PoseRecorder::~PoseRecorder() { Stop(); }

    bool PoseRecorder::Start(const std::string& name, const std::string& world) {
        if (m_active) Stop();
        if (name.empty()) {
            Log::Warning("[Record] a recording needs a name");
            return false;
        }
        m_name = name;
        m_path = Recording::PathFor(name);
        m_recording = Recording{};
        m_recording.world = world;
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char buf[64];
            std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &local);
            m_recording.recordedAt = buf;
        }
        m_recording.samples.reserve(1 << 16);
        m_haveStart = false;
        m_clock = 0.0;
        m_nextLogAt = 5.0;
        m_active = true;
        Log::Info("[Record] recording \"%s\" -> %s", m_name.c_str(), m_path.c_str());
        return true;
    }

    double PoseRecorder::Elapsed() const {
        return m_active ? m_clock : 0.0;
    }

    void PoseRecorder::Sample(const Game::ClientPlayer& player, const Render::Camera& camera,
                              int8_t dim, bool crossedThisFrame) {
        if (!m_active) return;
        const long long now = NowNs();
        if (!m_haveStart) { m_startNs = now; m_haveStart = true; }
        m_clock = static_cast<double>(now - m_startNs) * 1e-9;

        PoseSample s;
        s.t        = m_clock;
        s.dim      = dim;
        s.pos      = glm::dvec3(player.physics.position);
        s.yaw      = camera.yaw;
        s.pitch    = camera.pitch;
        s.scale    = player.physics.scale;
        s.speed    = player.physics.currentSpeed;
        s.sneak    = player.physics.isSneaking;
        s.sprint   = player.physics.isSprinting;
        s.fly      = player.physics.isFlying;
        s.onGround = player.physics.isOnGround;
        if (crossedThisFrame) {
            s.arrival = PoseSample::Arrival::PortalCrossing;
        } else if (!m_recording.samples.empty()) {
            const PoseSample& prev = m_recording.samples.back();
            if (prev.dim != dim || glm::distance(prev.pos, s.pos) > kTeleportJumpBlocks) {
                s.arrival = PoseSample::Arrival::Teleport;
                Log::Info("[Record] t=%.2fs teleport: dim %d (%.1f,%.1f,%.1f) -> dim %d (%.1f,%.1f,%.1f)",
                          s.t, static_cast<int>(prev.dim), prev.pos.x, prev.pos.y, prev.pos.z,
                          static_cast<int>(dim), s.pos.x, s.pos.y, s.pos.z);
            }
        }
        if (crossedThisFrame) {
            Log::Info("[Record] t=%.2fs portal crossing -> dim %d (%.1f,%.1f,%.1f)",
                      s.t, static_cast<int>(dim), s.pos.x, s.pos.y, s.pos.z);
        }
        m_recording.samples.push_back(s);

        if (m_clock >= m_nextLogAt) {
            m_nextLogAt += 5.0;
            Log::Info("[Record] t=%.0fs samples=%zu dim=%d pos=(%.1f,%.1f,%.1f) yaw=%.0f pitch=%.0f",
                      m_clock, m_recording.samples.size(), static_cast<int>(dim),
                      s.pos.x, s.pos.y, s.pos.z, s.yaw, s.pitch);
        }
    }

    void PoseRecorder::Stop() {
        if (!m_active) return;
        m_active = false;
        std::string error;
        if (m_recording.Save(m_path, error)) {
            Log::Info("[Record] saved \"%s\": %zu samples, %.1f s -> %s",
                      m_name.c_str(), m_recording.samples.size(), m_recording.Duration(),
                      m_path.c_str());
        } else {
            Log::Error("[Record] %s", error.c_str());
        }
    }

    // ── PoseReplayer ──────────────────────────────────────────────────

    bool PoseReplayer::Load(const std::string& name, double holdSeconds) {
        Stop();
        const std::string path = Recording::PathFor(name);
        std::string error;
        Recording rec;
        if (!rec.Load(path, error)) {
            Log::Error("[Replay] %s", error.c_str());
            return false;
        }
        m_name = name;
        m_recording = std::move(rec);
        m_holdSeconds = std::max(0.0, holdSeconds);
        m_holdElapsed = 0.0;
        m_clock = 0.0;
        m_cursor = 0;
        m_crossingSeen = false;
        m_waitLoggedAt = -1.0;
        m_stage = Stage::Hold;
        const PoseSample& first = m_recording.samples.front();
        Log::Info("[Replay] loaded \"%s\" (%s, world \"%s\"): %zu samples, %.1f s, "
                  "starts dim %d at (%.1f,%.1f,%.1f); holding %.1f s before playback",
                  name.c_str(), m_recording.recordedAt.c_str(), m_recording.world.c_str(),
                  m_recording.samples.size(), m_recording.Duration(), static_cast<int>(first.dim),
                  first.pos.x, first.pos.y, first.pos.z, m_holdSeconds);
        return true;
    }

    void PoseReplayer::Stop() {
        if (Active()) Log::Info("[Replay] \"%s\" stopped at t=%.2fs", m_name.c_str(), Time());
        m_stage = Stage::Idle;
    }

    double PoseReplayer::Time() const {
        switch (m_stage) {
            case Stage::Hold:    return m_holdElapsed - m_holdSeconds;
            case Stage::Playing: return m_clock;
            case Stage::Done:    return m_clock;
            default:             return 0.0;
        }
    }

    void PoseReplayer::Finish(const char* why) {
        Log::Info("[Replay] \"%s\" finished at t=%.2fs (%s)", m_name.c_str(), m_clock, why);
        m_stage = Stage::Done;
    }

    void PoseReplayer::Write(Game::ClientPlayer& player, Render::Camera& camera,
                             const PoseSample& s, const glm::dvec3& pos,
                             float yaw, float pitch, float dt) const {
        const glm::vec3 newPos(pos);
        if (dt > 0.0f) {
            player.physics.velocity = (newPos - player.physics.position) / dt;
        }
        player.physics.position     = newPos;
        player.physics.scale        = s.scale;
        player.physics.currentSpeed = s.speed;
        player.physics.isSneaking   = s.sneak;
        player.physics.isSprinting  = s.sprint;
        player.physics.isFlying     = s.fly;
        player.physics.isOnGround   = s.onGround;
        player.physics.fallDistance = 0.0f;
        player.predictedPos = pos;   // what UpdatePhysics does every frame
        player.yaw   = yaw;
        player.pitch = pitch;
        camera.yaw   = yaw;
        camera.pitch = pitch;
    }

    void PoseReplayer::Snap(Game::ClientPlayer& player, Render::Camera& camera,
                            const PoseSample& s) const {
        Write(player, camera, s, s.pos, s.yaw, s.pitch, 0.0f);
        player.physics.velocity = glm::vec3(0.0f);
        player.serverPos = s.pos;
        player.visualPos = s.pos;
    }

    void PoseReplayer::OnCrossing() {
        if (m_stage == Stage::Playing) m_crossingSeen = true;
    }

    void PoseReplayer::Apply(Game::ClientPlayer& player, Render::Camera& camera,
                             float dt, int8_t activeDim) {
        if (!Active()) return;
        const auto& S = m_recording.samples;

        if (m_stage == Stage::Hold) {
            const PoseSample& first = S.front();
            if (first.dim != activeDim) {
                Log::Error("[Replay] \"%s\" starts in dimension %d but the player is in %d; "
                           "load the world in the recording's dimension first",
                           m_name.c_str(), static_cast<int>(first.dim), static_cast<int>(activeDim));
                m_stage = Stage::Done;
                return;
            }
            if (m_holdElapsed == 0.0) {
                Log::Info("[Replay] snapped to first sample (%.1f,%.1f,%.1f) yaw %.0f pitch %.0f",
                          first.pos.x, first.pos.y, first.pos.z, first.yaw, first.pitch);
            }
            Snap(player, camera, first);
            m_holdElapsed += dt;
            PROFILE_PLOT("Replay/Time", Time());
            if (m_holdElapsed >= m_holdSeconds) {
                m_stage = Stage::Playing;
                m_clock = 0.0;
                Log::Info("[Replay] playing \"%s\" (%.1f s)", m_name.c_str(), Duration());
            }
            return;
        }

        // Playing.
        m_clock += dt;
        PROFILE_PLOT("Replay/Time", m_clock);

        // Advance over ordinary intervals; stop at a crossing or teleport
        // interval, which resolve on their own terms below.
        while (m_cursor + 1 < S.size() && S[m_cursor + 1].t <= m_clock &&
               S[m_cursor + 1].arrival == PoseSample::Arrival::Move) {
            ++m_cursor;
        }
        if (m_cursor + 1 >= S.size()) {
            Snap(player, camera, S.back());
            Finish("end of recording");
            return;
        }

        const PoseSample& a = S[m_cursor];
        const PoseSample& b = S[m_cursor + 1];

        switch (b.arrival) {
        case PoseSample::Arrival::Move: {
            const double span = b.t - a.t;
            const double f = span > 0.0 ? std::clamp((m_clock - a.t) / span, 0.0, 1.0) : 1.0;
            const glm::dvec3 pos = glm::mix(a.pos, b.pos, f);
            const float yaw   = LerpYaw(a.yaw, b.yaw, f);
            const float pitch = a.pitch + (b.pitch - a.pitch) * static_cast<float>(f);
            Write(player, camera, f < 0.5 ? a : b, pos, yaw, pitch, dt);
            return;
        }

        case PoseSample::Arrival::PortalCrossing: {
            if (m_crossingSeen) {
                // The traveler moved the player through; the far-side path
                // resumes from b. Written as a snap: the crossing code
                // already put the player within a nudge of it.
                m_crossingSeen = false;
                m_waitLoggedAt = -1.0;
                ++m_cursor;
                Snap(player, camera, b);
                Log::Info("[Replay] t=%.2fs portal crossing reproduced -> dim %d (%.1f,%.1f,%.1f)",
                          m_clock, static_cast<int>(b.dim), b.pos.x, b.pos.y, b.pos.z);
                return;
            }
            if (m_clock <= b.t + kCrossingGraceSec) {
                // Carry the pre-crossing motion straight on through the
                // surface: the eye path the traveler sees is the recorded
                // frame's, at whatever frame rate this run has.
                glm::dvec3 vel(0.0);
                if (m_cursor > 0) {
                    const PoseSample& p = S[m_cursor - 1];
                    const double span = a.t - p.t;
                    if (span > 1e-6) vel = (a.pos - p.pos) / span;
                }
                const glm::dvec3 pos = a.pos + vel * std::max(0.0, m_clock - a.t);
                Write(player, camera, a, pos, a.yaw, a.pitch, dt);
                return;
            }
            // The crossing never came (portal not loaded, path missed it).
            if (b.dim == activeDim) {
                Log::Warning("[Replay] t=%.2fs portal crossing did not reproduce; snapping to "
                             "the far side (%.1f,%.1f,%.1f)", m_clock, b.pos.x, b.pos.y, b.pos.z);
                ++m_cursor;
                Snap(player, camera, b);
                return;
            }
            Log::Error("[Replay] t=%.2fs portal crossing into dimension %d did not reproduce "
                       "(player still in %d); replay aborted", m_clock,
                       static_cast<int>(b.dim), static_cast<int>(activeDim));
            Finish("crossing failed");
            return;
        }

        case PoseSample::Arrival::Teleport: {
            if (m_clock < b.t) {
                // Hold the last pose: a teleport is not interpolated across.
                Write(player, camera, a, a.pos, a.yaw, a.pitch, dt);
                return;
            }
            if (b.dim == activeDim) {
                ++m_cursor;
                Snap(player, camera, b);
                Log::Info("[Replay] t=%.2fs teleport -> (%.1f,%.1f,%.1f)", m_clock,
                          b.pos.x, b.pos.y, b.pos.z);
                return;
            }
            // A dimension change the server has to make. Wait for it (a
            // vanilla portal the player stood in fires on the server's
            // clock), then give up.
            if (m_waitLoggedAt < 0.0) {
                m_waitLoggedAt = m_clock;
                Log::Warning("[Replay] t=%.2fs recording moves to dimension %d; waiting for the "
                             "server to send the player there", m_clock, static_cast<int>(b.dim));
            }
            Write(player, camera, a, a.pos, a.yaw, a.pitch, dt);
            if (m_clock > b.t + kTeleportGraceSec) {
                Log::Error("[Replay] t=%.2fs dimension change to %d never happened; replay aborted",
                           m_clock, static_cast<int>(b.dim));
                Finish("dimension change failed");
            }
            return;
        }
        }
    }

    void PoseReplayer::LogProgress() const {
        if (!Active()) return;
        const auto& S = m_recording.samples;
        const PoseSample& s = S[std::min(m_cursor, S.size() - 1)];
        Log::Info("[Replay] t=%.1fs/%.1fs %s sample=%zu/%zu dim=%d pos=(%.1f,%.1f,%.1f) yaw=%.0f pitch=%.0f",
                  Time(), Duration(), m_stage == Stage::Hold ? "hold" : "play",
                  m_cursor, S.size(), static_cast<int>(s.dim),
                  s.pos.x, s.pos.y, s.pos.z, s.yaw, s.pitch);
    }

} // namespace Client::Dev
