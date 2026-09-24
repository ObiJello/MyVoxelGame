// File: src/client/control/RemoteControl.cpp
#include "RemoteControl.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Client::Control {

    namespace {
        // How far behind the newest frame the picture runs. Three frames at
        // 60 Hz: enough to ride out ordinary jitter and the two clients'
        // frame rates beating against each other, small enough not to feel.
        constexpr double kPlaybackDelayMs = 50.0;
        constexpr double kKeepSamplesMs   = 1000.0;

        double LocalNowMs() {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        float LerpAngle(float a, float b, float t) {
            float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
            return a + d * t;
        }
    }

    State& Get() {
        static State s;
        return s;
    }

    void OnControl(const Network::ControlS2CPacket& packet) {
        State& s = Get();
        if (s.role == packet.role && s.otherId == packet.otherId) return;
        s.prevRole    = s.role;
        s.role        = packet.role;
        s.otherId     = packet.otherId;
        s.otherName   = packet.otherName;
        s.roleChanged = true;
        s.hasView     = false;
        s.pendingInput.clear();
        s.remoteCursorValid = false;
        s.cursorActive = false;
        s.mirrored = Network::ControlScreen::None;
        s.sentReleaseForLocalUi = false;
        Log::Info("[Control] role %u, other %u (%s)",
                  static_cast<unsigned>(packet.role), packet.otherId, packet.otherName.c_str());
    }

    void OnInput(const Network::ControlInputPacket& packet) {
        State& s = Get();
        if (s.role != Network::ControlRole::Controlled) return;
        // A stalled frame must not bank an unbounded backlog of frames that
        // would then all replay at once.
        if (s.pendingInput.size() < 64) s.pendingInput.push_back(packet);
    }

    void OnView(const Network::ControlViewPacket& packet) {
        State& s = Get();
        if (!ReceivesView()) return;

        // Unwrap the sender's 32-bit clock.
        double sendMs = static_cast<double>(packet.sendTimeMs) + s.sendTimeBaseMs;
        if (!s.samples.empty() && sendMs < s.lastSendTimeMs - 2.0e9) {
            s.sendTimeBaseMs += 4294967296.0;
            sendMs += 4294967296.0;
        }
        if (!s.samples.empty() && sendMs < s.lastSendTimeMs) return;   // out of order: drop
        s.lastSendTimeMs = sendMs;

        // Clock offset: the smallest local-minus-sender seen is the one with
        // the least queueing in it. Track down at once, up slowly (so a
        // drifting clock or a rising floor is followed, not a spike).
        const double measured = LocalNowMs() - sendMs;
        if (!s.clockOffsetValid) {
            s.clockOffsetMs    = measured;
            s.clockOffsetValid = true;
        } else if (measured < s.clockOffsetMs) {
            s.clockOffsetMs = measured;
        } else {
            s.clockOffsetMs += (measured - s.clockOffsetMs) * 0.005;
        }

        s.samples.push_back({sendMs, packet});
        if (packet.swing) s.pendingSwing = true;
        if (!s.hasView) {
            s.view    = packet;
            s.hasView = true;
        }
    }

    void UpdateView(double localNowMs) {
        State& s = Get();
        if (!ReceivesView() || s.samples.empty() || !s.clockOffsetValid) return;

        const double t = localNowMs - s.clockOffsetMs - kPlaybackDelayMs;   // sender clock

        // Drop what is well behind the playback point, keeping one sample
        // at or before it to interpolate from.
        while (s.samples.size() > 1 && s.samples[1].sendTimeMs <= t - kKeepSamplesMs) {
            s.samples.erase(s.samples.begin());
        }

        const auto& newest = s.samples.back();
        if (t >= newest.sendTimeMs) {
            // Ahead of everything received: hold the newest frame.
            s.view = newest.view;
            return;
        }
        // Bracket t.
        size_t i = 0;
        while (i + 1 < s.samples.size() && s.samples[i + 1].sendTimeMs <= t) ++i;
        if (i + 1 >= s.samples.size()) { s.view = newest.view; return; }
        const auto& a = s.samples[i];
        const auto& b = s.samples[i + 1];
        if (t < a.sendTimeMs) { s.view = a.view; return; }
        const double span = b.sendTimeMs - a.sendTimeMs;
        const float f = span > 1.0e-6 ? static_cast<float>(std::clamp((t - a.sendTimeMs) / span, 0.0, 1.0)) : 1.0f;

        Network::ControlViewPacket v = b.view;   // discrete fields: the later frame
        if (a.view.perspective == b.view.perspective && a.view.dimension == b.view.dimension) {
            v.cameraPos = a.view.cameraPos + (b.view.cameraPos - a.view.cameraPos) * static_cast<double>(f);
            v.playerPos = a.view.playerPos + (b.view.playerPos - a.view.playerPos) * static_cast<double>(f);
            v.yaw   = LerpAngle(a.view.yaw, b.view.yaw, f);
            v.pitch = a.view.pitch + (b.view.pitch - a.view.pitch) * f;
            v.fov   = a.view.fov + (b.view.fov - a.view.fov) * f;
        }
        s.view = v;
    }

    void Reset() {
        State& s = Get();
        s = State{};
    }

} // namespace Client::Control
