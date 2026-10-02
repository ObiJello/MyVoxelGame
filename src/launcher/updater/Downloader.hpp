// File: src/launcher/updater/Downloader.hpp
#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <cstdint>

namespace Launcher {

    class Downloader {
    public:
        using ProgressCallback = std::function<void(size_t bytesDownloaded, size_t totalBytes)>;
        // Sees every byte of the body in order, on the download thread, right after
        // it is written to the file (the streaming extractor reads the zip from here).
        using DataSink = std::function<void(const uint8_t* data, size_t size)>;

        // Download a file from url to outputPath. Calls progress callback periodically.
        // Returns true on success.
        bool Download(const std::string& url, const std::string& outputPath, ProgressCallback progress = nullptr,
                      DataSink sink = nullptr);

        // Cancel an in-progress download
        void Cancel();

    private:
        std::atomic<bool> m_cancelled{false};
    };

} // namespace Launcher
