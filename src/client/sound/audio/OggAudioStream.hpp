// File: src/client/sound/audio/OggAudioStream.hpp
//
// MC net.minecraft.client.sounds.AudioStream / JOrbisAudioStream /
// LoopingAudioStream, over stb_vorbis instead of JOrbis.
//
//   OggAudioStream  a pull decoder: Read(n) hands back up to n bytes of
//                   16-bit interleaved PCM, empty at the end of the stream.
//                   With `looping` it is MC's LoopingAudioStream — at the end
//                   it rewinds and keeps going, so a streamed music loop never
//                   runs dry.
//   DecodeOggFile   the whole file at once — MC SoundBufferLibrary's
//                   getCompleteBuffer (FiniteAudioStream.readAll), for the
//                   static (non-streamed) sounds.
//
// The file is read into memory first (std::filesystem paths, so non-ASCII
// install locations work on Windows) and decoded from there; stb_vorbis keeps
// a pointer into that buffer, which the stream owns.
//
// Channels are kept as the file has them, as MC does: the vanilla effect
// files are mono (so OpenAL can place them) and the music is stereo (played
// relative, unplaced).
#pragma once

#include "client/sound/audio/OpenAlUtil.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct stb_vorbis;

namespace Client::Audio {

    class AudioStream {
    public:
        virtual ~AudioStream() = default;
        virtual const AudioFormat& GetFormat() const = 0;
        // MC AudioStream.read(expectedSize).
        virtual std::vector<int16_t> Read(size_t expectedBytes) = 0;
    };

    class OggAudioStream final : public AudioStream {
    public:
        // Null (and `error` filled) when the file cannot be read or is not
        // Ogg Vorbis.
        static std::unique_ptr<OggAudioStream> Open(const std::string& path, bool looping,
                                                    std::string& error);
        ~OggAudioStream() override;

        OggAudioStream(const OggAudioStream&) = delete;
        OggAudioStream& operator=(const OggAudioStream&) = delete;

        const AudioFormat& GetFormat() const override { return m_format; }
        std::vector<int16_t> Read(size_t expectedBytes) override;

    private:
        OggAudioStream() = default;

        std::vector<uint8_t> m_file;
        stb_vorbis*          m_vorbis = nullptr;
        AudioFormat          m_format;
        bool                 m_looping = false;
    };

    // Whole-file decode for a static buffer. False (and `error`) on failure.
    bool DecodeOggFile(const std::string& path, std::vector<int16_t>& pcm, AudioFormat& format,
                       std::string& error);

    // The file's bytes. False when it cannot be opened.
    bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out);

} // namespace Client::Audio
