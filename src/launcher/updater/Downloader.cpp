// File: src/launcher/updater/Downloader.cpp
#include "Downloader.hpp"
#include "launcher/LauncherConfig.hpp"
#include "common/core/Log.hpp"
#include <curl/curl.h>
#include <fstream>
#include <filesystem>

namespace Launcher {

    struct DownloadContext {
        std::ofstream file;
        Downloader::ProgressCallback progressCallback;
        std::atomic<bool>* cancelled;
    };

    static size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userdata) {
        auto* ctx = static_cast<DownloadContext*>(userdata);
        if (ctx->cancelled && ctx->cancelled->load()) {
            return 0; // Returning 0 aborts the transfer
        }
        size_t totalSize = size * nmemb;
        ctx->file.write(static_cast<char*>(contents), static_cast<std::streamsize>(totalSize));
        return ctx->file.good() ? totalSize : 0;
    }

    static int CurlProgressCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                                     curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
        auto* ctx = static_cast<DownloadContext*>(userdata);
        if (ctx->cancelled && ctx->cancelled->load()) {
            return 1; // Non-zero cancels the transfer
        }
        if (ctx->progressCallback && dltotal > 0) {
            ctx->progressCallback(static_cast<size_t>(dlnow), static_cast<size_t>(dltotal));
        }
        return 0;
    }

    bool Downloader::Download(const std::string& url, const std::string& outputPath, ProgressCallback progress) {
        m_cancelled = false;

        // Ensure parent directory exists
        auto parentDir = std::filesystem::path(outputPath).parent_path();
        if (!parentDir.empty()) {
            std::filesystem::create_directories(parentDir);
        }

        // Download to a temp file first
        std::string tmpPath = outputPath + ".tmp";

        Log::Info("Downloading: %s", url.c_str());
        Log::Info("Saving to: %s", outputPath.c_str());

        CURL* curl = curl_easy_init();
        if (!curl) {
            Log::Error("Failed to initialize curl for download");
            return false;
        }

        DownloadContext ctx;
        ctx.file.open(tmpPath, std::ios::binary);
        if (!ctx.file.is_open()) {
            Log::Error("Failed to open file for writing: %s", tmpPath.c_str());
            curl_easy_cleanup(curl);
            return false;
        }
        ctx.progressCallback = progress;
        ctx.cancelled = &m_cancelled;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/octet-stream");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L); // 10 minute timeout for large files
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        ctx.file.close();

        if (m_cancelled) {
            Log::Info("Download cancelled");
            std::filesystem::remove(tmpPath);
            return false;
        }

        if (res != CURLE_OK) {
            Log::Error("Download failed: %s", curl_easy_strerror(res));
            std::filesystem::remove(tmpPath);
            return false;
        }

        if (httpCode != 200) {
            Log::Error("Download HTTP error: %ld", httpCode);
            std::filesystem::remove(tmpPath);
            return false;
        }

        // Rename tmp to final
        std::error_code ec;
        std::filesystem::rename(tmpPath, outputPath, ec);
        if (ec) {
            Log::Error("Failed to rename download: %s", ec.message().c_str());
            std::filesystem::remove(tmpPath);
            return false;
        }

        auto fileSize = std::filesystem::file_size(outputPath);
        Log::Info("Download complete: %s (%.1f MB)", outputPath.c_str(),
                  static_cast<double>(fileSize) / (1024.0 * 1024.0));
        return true;
    }

    void Downloader::Cancel() {
        m_cancelled = true;
    }

} // namespace Launcher
