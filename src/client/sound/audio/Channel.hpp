// File: src/client/sound/audio/Channel.hpp
//
// MC com.mojang.blaze3d.audio.Channel: one AL source, playing either a static
// buffer or a stream fed four one-second buffers at a time.
//
// Sound executor thread only (see client/sound/ChannelAccess).
#pragma once

#include "client/sound/audio/OggAudioStream.hpp"
#include "client/sound/audio/SoundBuffer.hpp"

#include <glm/glm.hpp>

#include <atomic>
#include <memory>

namespace Client::Audio {

    class Channel {
    public:
        static constexpr int kQueuedBufferCount = 4;
        static constexpr int kBufferDurationSeconds = 1;

        // Null (and logged) when AL has no source to give.
        static std::unique_ptr<Channel> Create();
        ~Channel();

        Channel(const Channel&) = delete;
        Channel& operator=(const Channel&) = delete;

        void Destroy();
        void Play();
        void Pause();
        void Unpause();
        void Stop();
        bool Playing() const;
        bool Stopped() const;

        void SetSelfPosition(const glm::dvec3& position);
        void SetPitch(float pitch);
        void SetLooping(bool looping);
        void SetVolume(float volume);
        void DisableAttenuation();
        // MC linearAttenuation: full volume at the source, silent at
        // `maxDistance`, a straight line between.
        void LinearAttenuation(float maxDistance);
        void SetRelative(bool relative);

        void AttachStaticBuffer(SoundBuffer& buffer);
        void AttachBufferStream(std::shared_ptr<AudioStream> stream);
        // Refill the stream's played-out buffers. Called every tick.
        void UpdateStream();

    private:
        explicit Channel(ALuint source) : m_source(source) {}

        ALint GetState() const;
        void  PumpBuffers(int count);
        int   RemoveProcessedBuffers();

        ALuint                       m_source = 0;
        std::atomic<bool>            m_initialized{true};
        int                          m_streamingBufferSize = 16384;
        std::shared_ptr<AudioStream> m_stream;
    };

} // namespace Client::Audio
