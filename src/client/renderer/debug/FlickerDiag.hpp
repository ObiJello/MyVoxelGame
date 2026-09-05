// File: src/client/renderer/debug/FlickerDiag.hpp
//
// Flicker detector. "Something disappears and reappears really fast" is
// a value that goes A → B → A across consecutive frames: a portal drawn,
// not drawn, drawn; a section count 1800, 40, 1800; an occlusion source
// exact, fallback, exact. Renderers Record() the values that could do
// that, once per frame, and EndFrame() logs every key that toggled this
// frame in ONE line with the camera for context — so the log shows what
// flickered, and what else changed in the same frame, without guessing.
//
// Opt-in: OBEY_FLICKER_DIAG=1. Off, every call is a cached bool test.
//
//   Record(key, value)      — a per-frame measurement; only A→B→A logs.
//   RecordState(key, value) — a mode; EVERY change logs (rate-limited).
//   Note(key, text)         — context appended to any line logged this frame.
#pragma once

#include <cstdint>
#include <string>

namespace Render::FlickerDiag {

    bool Enabled();
    void Record(const std::string& key, int64_t value);
    void RecordState(const std::string& key, int64_t value);
    void Note(const std::string& key, const std::string& text);
    void EndFrame();

} // namespace Render::FlickerDiag
