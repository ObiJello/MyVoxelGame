// File: src/launcher/updater/Downloader.hpp
#pragma once

#include <string>
#include <functional>
#include <atomic>

namespace Launcher {

    class Downloader {
    public:
        using ProgressCallback = std::function<void(size_t bytesDownloaded, size_t totalBytes)>;

        // Download a file from url to outputPath. Calls progress callback periodically.
        // Returns true on success.
        bool Download(const std::string& url, const std::string& outputPath, ProgressCallback progress = nullptr);

        // Cancel an in-progress download
        void Cancel();

    private:
        std::atomic<bool> m_cancelled{false};
    };

} // namespace Launcher
