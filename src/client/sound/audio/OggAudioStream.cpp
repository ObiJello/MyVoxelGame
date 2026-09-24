// File: src/client/sound/audio/OggAudioStream.cpp
#include "client/sound/audio/OggAudioStream.hpp"

#include <stb_vorbis.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace Client::Audio {

    bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
        std::error_code ec;
        // UTF-8 in, whatever the platform's native path encoding is out
        // (std::filesystem::u8path without its C++20 deprecation).
        const std::filesystem::path p(std::u8string(path.begin(), path.end()));
        const auto size = std::filesystem::file_size(p, ec);
        if (ec || size == 0 || size > static_cast<std::uintmax_t>(INT_MAX)) return false;
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        out.resize(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        return static_cast<std::uintmax_t>(in.gcount()) == size;
    }

    std::unique_ptr<OggAudioStream> OggAudioStream::Open(const std::string& path, bool looping,
                                                         std::string& error) {
        std::unique_ptr<OggAudioStream> stream(new OggAudioStream());
        if (!ReadFileBytes(path, stream->m_file)) {
            error = "cannot read " + path;
            return nullptr;
        }
        int err = 0;
        stream->m_vorbis = stb_vorbis_open_memory(stream->m_file.data(),
                                                  static_cast<int>(stream->m_file.size()),
                                                  &err, nullptr);
        if (!stream->m_vorbis) {
            error = "invalid Ogg Vorbis (" + std::to_string(err) + "): " + path;
            return nullptr;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(stream->m_vorbis);
        stream->m_format.sampleRate = static_cast<int>(info.sample_rate);
        stream->m_format.channels   = std::clamp(info.channels, 1, 2);
        stream->m_looping = looping;
        return stream;
    }

    OggAudioStream::~OggAudioStream() {
        if (m_vorbis) stb_vorbis_close(m_vorbis);
    }

    std::vector<int16_t> OggAudioStream::Read(size_t expectedBytes) {
        const int channels = m_format.channels;
        const size_t wantShorts = std::max<size_t>(expectedBytes / 2, static_cast<size_t>(channels));
        std::vector<int16_t> out(wantShorts - wantShorts % static_cast<size_t>(channels));
        size_t filled = 0;
        bool rewoundOnce = false;
        while (filled < out.size()) {
            const int frames = stb_vorbis_get_samples_short_interleaved(
                m_vorbis, channels, out.data() + filled, static_cast<int>(out.size() - filled));
            if (frames > 0) {
                filled += static_cast<size_t>(frames) * static_cast<size_t>(channels);
                rewoundOnce = false;
                continue;
            }
            // End of the stream. MC LoopingAudioStream: rewind and read on —
            // but only once per empty read, so a file that decodes to nothing
            // cannot spin here.
            if (!m_looping || rewoundOnce) break;
            stb_vorbis_seek_start(m_vorbis);
            rewoundOnce = true;
        }
        out.resize(filled);
        return out;
    }

    bool DecodeOggFile(const std::string& path, std::vector<int16_t>& pcm, AudioFormat& format,
                       std::string& error) {
        std::vector<uint8_t> file;
        if (!ReadFileBytes(path, file)) {
            error = "cannot read " + path;
            return false;
        }
        int channels = 0, sampleRate = 0;
        short* data = nullptr;
        const int frames = stb_vorbis_decode_memory(file.data(), static_cast<int>(file.size()),
                                                    &channels, &sampleRate, &data);
        if (frames < 0 || !data || channels <= 0) {
            if (data) std::free(data);
            error = "invalid Ogg Vorbis: " + path;
            return false;
        }
        const int keep = std::clamp(channels, 1, 2);
        pcm.resize(static_cast<size_t>(frames) * static_cast<size_t>(keep));
        if (keep == channels) {
            std::copy(data, data + pcm.size(), pcm.begin());
        } else {
            // More than two channels: keep the first two (OpenAL has no
            // positional multichannel, and nothing vanilla ships is >2).
            for (int f = 0; f < frames; ++f) {
                pcm[static_cast<size_t>(f) * 2]     = data[static_cast<size_t>(f) * channels];
                pcm[static_cast<size_t>(f) * 2 + 1] = data[static_cast<size_t>(f) * channels + 1];
            }
        }
        std::free(data);
        format.sampleRate = sampleRate;
        format.channels   = keep;
        return true;
    }

} // namespace Client::Audio
