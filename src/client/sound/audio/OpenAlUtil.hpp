// File: src/client/sound/audio/OpenAlUtil.hpp
//
// MC com.mojang.blaze3d.audio.OpenAlUtil: error checks that log and report,
// and the PCM format → AL format mapping.
#pragma once

#include <AL/al.h>
#include <AL/alc.h>

namespace Client::Audio {

    // PCM as this engine decodes it: signed 16-bit, interleaved. MC's
    // javax AudioFormat reduced to the two numbers that vary.
    struct AudioFormat {
        int sampleRate = 44100;
        int channels   = 1;
        int BytesPerFrame() const { return channels * 2; }
    };

    // True (and logged) when an AL error is pending. `location` names the
    // call, as MC's checkALError(location).
    bool CheckALError(const char* location);
    bool CheckALCError(ALCdevice* device, const char* location);

    // MC audioFormatToOpenAl: 16-bit mono / stereo. Anything else is refused
    // (returns 0) rather than thrown; the decoder never produces it.
    ALenum AudioFormatToOpenAl(const AudioFormat& format);

} // namespace Client::Audio
