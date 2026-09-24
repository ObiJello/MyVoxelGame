// File: src/client/sound/audio/Channel.cpp
#include "client/sound/audio/Channel.hpp"

#include <AL/alext.h>

#include <vector>

namespace Client::Audio {

    std::unique_ptr<Channel> Channel::Create() {
        ALuint id = 0;
        alGenSources(1, &id);
        if (CheckALError("Allocate new source")) return nullptr;
        return std::unique_ptr<Channel>(new Channel(id));
    }

    Channel::~Channel() { Destroy(); }

    void Channel::Destroy() {
        bool expected = true;
        if (m_initialized.compare_exchange_strong(expected, false)) {
            alSourceStop(m_source);
            CheckALError("Stop");
            if (m_stream) {
                // A stopped source has played out (processed) everything it
                // had queued, so this frees every stream buffer. A static
                // buffer is never touched: it belongs to SoundBufferLibrary.
                m_stream.reset();
                RemoveProcessedBuffers();
            }
            alDeleteSources(1, &m_source);
            CheckALError("Cleanup");
        }
    }

    void Channel::Play() { alSourcePlay(m_source); }

    ALint Channel::GetState() const {
        if (!m_initialized.load()) return AL_STOPPED;
        ALint state = AL_STOPPED;
        alGetSourcei(m_source, AL_SOURCE_STATE, &state);
        return state;
    }

    void Channel::Pause() {
        if (GetState() == AL_PLAYING) alSourcePause(m_source);
    }

    void Channel::Unpause() {
        if (GetState() == AL_PAUSED) alSourcePlay(m_source);
    }

    void Channel::Stop() {
        if (m_initialized.load()) {
            alSourceStop(m_source);
            CheckALError("Stop");
        }
    }

    bool Channel::Playing() const { return GetState() == AL_PLAYING; }
    bool Channel::Stopped() const { return GetState() == AL_STOPPED; }

    void Channel::SetSelfPosition(const glm::dvec3& p) {
        alSource3f(m_source, AL_POSITION, static_cast<float>(p.x), static_cast<float>(p.y),
                   static_cast<float>(p.z));
    }

    void Channel::SetPitch(float pitch) { alSourcef(m_source, AL_PITCH, pitch); }
    void Channel::SetLooping(bool looping) { alSourcei(m_source, AL_LOOPING, looping ? AL_TRUE : AL_FALSE); }
    void Channel::SetVolume(float volume) { alSourcef(m_source, AL_GAIN, volume); }

    void Channel::DisableAttenuation() {
        alSourcei(m_source, AL_DISTANCE_MODEL, AL_NONE);
    }

    void Channel::LinearAttenuation(float maxDistance) {
        alSourcei(m_source, AL_DISTANCE_MODEL, AL_LINEAR_DISTANCE);
        alSourcef(m_source, AL_MAX_DISTANCE, maxDistance);
        alSourcef(m_source, AL_ROLLOFF_FACTOR, 1.0f);
        alSourcef(m_source, AL_REFERENCE_DISTANCE, 0.0f);
    }

    void Channel::SetRelative(bool relative) {
        alSourcei(m_source, AL_SOURCE_RELATIVE, relative ? AL_TRUE : AL_FALSE);
    }

    void Channel::AttachStaticBuffer(SoundBuffer& buffer) {
        if (const auto id = buffer.GetAlBuffer()) {
            alSourcei(m_source, AL_BUFFER, static_cast<ALint>(*id));
        }
    }

    void Channel::AttachBufferStream(std::shared_ptr<AudioStream> stream) {
        if (!stream) return;
        m_stream = std::move(stream);
        const AudioFormat& f = m_stream->GetFormat();
        // MC calculateBufferSize: one second of PCM.
        m_streamingBufferSize = kBufferDurationSeconds * f.sampleRate * f.BytesPerFrame();
        PumpBuffers(kQueuedBufferCount);
    }

    void Channel::PumpBuffers(int count) {
        if (!m_stream) return;
        for (int i = 0; i < count; ++i) {
            std::vector<int16_t> pcm = m_stream->Read(static_cast<size_t>(m_streamingBufferSize));
            if (pcm.empty()) continue;
            SoundBuffer buffer(std::move(pcm), m_stream->GetFormat());
            if (const auto id = buffer.ReleaseAlBuffer()) {
                const ALuint bufferId = *id;
                alSourceQueueBuffers(m_source, 1, &bufferId);
            }
        }
    }

    void Channel::UpdateStream() {
        if (m_stream) {
            const int processed = RemoveProcessedBuffers();
            PumpBuffers(processed);
        }
    }

    int Channel::RemoveProcessedBuffers() {
        ALint processed = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
        if (processed > 0) {
            std::vector<ALuint> ids(static_cast<size_t>(processed));
            alSourceUnqueueBuffers(m_source, processed, ids.data());
            CheckALError("Unqueue buffers");
            alDeleteBuffers(processed, ids.data());
            CheckALError("Remove processed buffers");
        }
        return processed;
    }

} // namespace Client::Audio
