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
    // In-game switch (chat "/portaldiag on|off") — the launcher cannot set
    // the environment variable.
    void SetEnabled(bool on);
    // "/portaldiag mark": for the next `frames` frames every portal
    // renderer logs its full geometry and pass numbers for each portal in
    // view, whether or not anything toggled — the snapshot to read when
    // "it is happening right now".
    void RequestDump(int frames = 1);
    bool DumpPending();
    // "/portaldiag fill": every portal view is painted solid magenta and the
    // far world is not drawn — separates "the mask is wrong" (magenta shows
    // the same fault) from "the far view draws the wrong thing" (magenta is
    // solid).
    void SetDebugFill(bool on);
    bool DebugFill();
    void Record(const std::string& key, int64_t value);
    void RecordState(const std::string& key, int64_t value);
    void Note(const std::string& key, const std::string& text);
    void EndFrame();

} // namespace Render::FlickerDiag
