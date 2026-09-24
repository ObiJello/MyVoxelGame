// File: src/client/sound/audio/SoundBuffer.cpp
#include "client/sound/audio/SoundBuffer.hpp"

namespace Client::Audio {

    SoundBuffer::~SoundBuffer() {
        // An AL buffer still owned here is deleted by SoundBufferLibrary.Clear
        // (DiscardAlBuffer) while the context is alive; the destructor never
        // makes an AL call, because it may run after the context is gone.
    }

    std::optional<ALuint> SoundBuffer::GetAlBuffer() {
        if (!m_hasAlBuffer) {
            if (m_data.empty()) return std::nullopt;
            const ALenum format = AudioFormatToOpenAl(m_format);
            if (format == 0) return std::nullopt;
            ALuint id = 0;
            alGenBuffers(1, &id);
            if (CheckALError("Creating buffer")) return std::nullopt;
            alBufferData(id, format, m_data.data(), static_cast<ALsizei>(m_data.size() * sizeof(int16_t)),
                         m_format.sampleRate);
            if (CheckALError("Assigning buffer data")) {
                alDeleteBuffers(1, &id);
                return std::nullopt;
            }
            m_alBuffer = id;
            m_hasAlBuffer = true;
            m_data.clear();
            m_data.shrink_to_fit();
        }
        return m_alBuffer;
    }

    void SoundBuffer::DiscardAlBuffer() {
        if (m_hasAlBuffer) {
            alDeleteBuffers(1, &m_alBuffer);
            if (CheckALError("Deleting stream buffers")) return;
        }
        m_hasAlBuffer = false;
    }

    std::optional<ALuint> SoundBuffer::ReleaseAlBuffer() {
        std::optional<ALuint> result = GetAlBuffer();
        m_hasAlBuffer = false;
        return result;
    }

} // namespace Client::Audio
