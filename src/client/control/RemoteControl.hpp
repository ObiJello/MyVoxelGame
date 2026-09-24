// File: src/client/control/RemoteControl.hpp
//
// Client state for `/control` (the model is described in
// common/network/packets/game/ControlPackets.hpp). This module owns the
// role and the packets in flight; the per-frame work — feeding the Input
// layer, mirroring the view, sending the frames — is done in PlatformMain,
// which has the window, camera, player and screens in hand.
#pragma once

#include "common/network/packets/game/ControlPackets.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Client::Control {

    struct State {
        Network::ControlRole role = Network::ControlRole::None;
        Network::ControlRole prevRole = Network::ControlRole::None;   // the role a change came from
        uint32_t    otherId = 0;
        std::string otherName;
        // Set by a ControlS2C; PlatformMain reads and clears it to run the
        // start / stop transitions on the frame after the packet.
        bool roleChanged = false;

        // ── Controller side ──────────────────────────────────────────
        bool hasView = false;
        // The frame being SHOWN: interpolated by UpdateView from the
        // samples below, a fixed delay behind the newest one.
        Network::ControlViewPacket view;
        struct ViewSample {
            double sendTimeMs = 0.0;   // the sender's clock, unwrapped
            Network::ControlViewPacket view;
        };
        std::vector<ViewSample> samples;   // oldest first
        double clockOffsetMs = 0.0;        // localMs - senderMs, tracked toward its minimum
        bool   clockOffsetValid = false;
        double lastSendTimeMs = 0.0;       // for unwrapping the 32-bit sender clock
        double sendTimeBaseMs = 0.0;
        bool   cursorActive = false;   // a mirrored screen is up and the real cursor is being sent
        // Controlled: a hand swing happened since the last view frame.
        // Controller: one arrived and the hand has not played it yet.
        bool   pendingSwing = false;
        // What this client currently has open as the mirror of the
        // controlled client's own screen (survival / creative inventory,
        // chat); container screens come through the server's tee.
        Network::ControlScreen mirrored = Network::ControlScreen::None;
        // This client's own look, restored when control ends.
        float ownYaw = 0.0f;
        float ownPitch = 0.0f;
        bool  sentReleaseForLocalUi = false;

        // ── Controlled side ──────────────────────────────────────────
        std::vector<Network::ControlInputPacket> pendingInput;  // applied at the next frame start
        bool    remoteCursorValid = false;
        uint8_t remoteCursorRef = 0;  // see ControlInputPacket::cursorRef
        float   remoteCursorX = 0.0f; // GUI pixels
        float   remoteCursorY = 0.0f;
    };

    State& Get();

    inline bool IsControlling() { return Get().role == Network::ControlRole::Controller; }
    inline bool IsControlled()  { return Get().role == Network::ControlRole::Controlled; }
    inline bool IsHudMirroring(){ return Get().role == Network::ControlRole::HudMirror; }
    inline bool IsWatched()     { return Get().role == Network::ControlRole::Watched; }
    // Frames from another client are being played back (controller or
    // carried player).
    inline bool ReceivesView()  { return IsControlling() || IsHudMirroring(); }
    inline bool Active()        { return Get().role != Network::ControlRole::None; }

    // Packet entry points (main thread, from ClientPacketHandler).
    void OnControl(const Network::ControlS2CPacket& packet);
    void OnInput(const Network::ControlInputPacket& packet);
    void OnView(const Network::ControlViewPacket& packet);

    // Controller side, once per frame before the view is read: interpolate
    // `view` for the local time `localNowMs` (std::chrono::steady_clock, ms).
    void UpdateView(double localNowMs);

    // Leaving the world: forget everything.
    void Reset();

} // namespace Client::Control
