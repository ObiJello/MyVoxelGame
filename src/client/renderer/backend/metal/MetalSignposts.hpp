// File: src/client/renderer/backend/metal/MetalSignposts.hpp
//
// os_signpost intervals for Instruments (Metal System Trace + the
// os_signpost instrument, subsystem com.obeycraft.metal): where the CPU is
// in a frame while the GPU tracks show what it executes. Free unless a
// trace is recording (os_signpost_enabled is one load), and nothing here is
// Objective-C — plain C++ over <os/signpost.h>.
//
//   Frame    "Frame"      BeginFrame -> commit            id = frame number
//            "FrameWait"  the frame-slot semaphore wait   id = frame number + 1
//            "GPU"        scheduled -> completed handler  id = frame number
//   Encode   "Encode"     an encoder's recording          id = frame << 8 | ordinal
//   Present  "NextDrawable", "Present" (presentDrawable -> presented handler),
//            "Unpresented" (event: the mailbox kept the frame)
#pragma once

#ifdef __APPLE__
#include <os/signpost.h>
#include <cstdint>

namespace Render::MtlSignpost {

    inline os_log_t FrameLog()   { static os_log_t l = os_log_create("com.obeycraft.metal", "Frame");   return l; }
    inline os_log_t EncodeLog()  { static os_log_t l = os_log_create("com.obeycraft.metal", "Encode");  return l; }
    inline os_log_t PresentLog() { static os_log_t l = os_log_create("com.obeycraft.metal", "Present"); return l; }

    inline bool Enabled() { return os_signpost_enabled(FrameLog()); }

    // Signpost ids must be non-zero and not ~0; frame numbers start at 1.
    inline os_signpost_id_t FrameId(uint64_t frame) { return static_cast<os_signpost_id_t>(frame); }
    inline os_signpost_id_t EncoderId(uint64_t frame, uint32_t ordinal) {
        return static_cast<os_signpost_id_t>((frame << 8) | (ordinal & 0xFFu));
    }

} // namespace Render::MtlSignpost
#endif
