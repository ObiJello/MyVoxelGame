// File: src/client/sound/audio/SoundBuffer.hpp
//
// MC com.mojang.blaze3d.audio.SoundBuffer: decoded PCM that becomes an AL
// buffer the first time a channel attaches it, after which the PCM is freed.
//
// Threads: constructed wherever the decode ran (a decoder worker); every AL
// call — GetAlBuffer, DiscardAlBuffer, ReleaseAlBuffer — happens on the sound
// executor thread (or on the main thread once the executor is stopped, as
// MC's destroy path does).
#pragma once

#include "client/sound/audio/OpenAlUtil.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace Client::Audio {

    class SoundBuffer {
    public:
        SoundBuffer(std::vector<int16_t> data, const AudioFormat& format)
            : m_data(std::move(data)), m_format(format),
              m_size(static_cast<int>(m_data.size() * sizeof(int16_t))) {}
        ~SoundBuffer();

        SoundBuffer(const SoundBuffer&) = delete;
        SoundBuffer& operator=(const SoundBuffer&) = delete;

        // The AL buffer, uploading the PCM on first use.
        std::optional<ALuint> GetAlBuffer();
        void DiscardAlBuffer();
        // Hand the buffer to a streaming source, which deletes it once played
        // (MC releaseAlBuffer).
        std::optional<ALuint> ReleaseAlBuffer();

        const AudioFormat& Format() const { return m_format; }
        int  Size() const { return m_size; }
        bool IsValid() const { return !m_data.empty() || m_hasAlBuffer; }

    private:
        std::vector<int16_t> m_data;
        AudioFormat          m_format;
        bool                 m_hasAlBuffer = false;
        ALuint               m_alBuffer = 0;
        int                  m_size = 0;
    };

} // namespace Client::Audio
