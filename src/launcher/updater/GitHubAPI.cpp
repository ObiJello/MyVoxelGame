// File: src/launcher/updater/GitHubAPI.cpp
#include "GitHubAPI.hpp"
#include "launcher/LauncherConfig.hpp"
#include "common/core/Log.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace Launcher {

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
        size_t totalSize = size * nmemb;
        output->append(static_cast<char*>(contents), totalSize);
        return totalSize;
    }

    GitHubAPI::GitHubAPI(const std::string& owner, const std::string& repo)
        : m_owner(owner), m_repo(repo) {}

    bool GitHubAPI::FetchLatestRelease(ReleaseInfo& outInfo) {
        std::string url = std::string(GitHubAPIBase) + "/repos/" + m_owner + "/" + m_repo + "/releases/latest";

        Log::Info("Checking for updates: %s", url.c_str());

        CURL* curl = curl_easy_init();
        if (!curl) {
            Log::Error("Failed to initialize curl");
            return false;
        }

        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/vnd.github.v3+json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            Log::Error("GitHub API request failed: %s", curl_easy_strerror(res));
            return false;
        }

        if (httpCode != 200) {
            Log::Error("GitHub API returned HTTP %ld", httpCode);
            return false;
        }

        // Parse JSON response
        try {
            auto json = nlohmann::json::parse(response);

            outInfo.tagName = json.value("tag_name", "");
            outInfo.name = json.value("name", "");
            outInfo.body = json.value("body", "");
            outInfo.publishedAt = json.value("published_at", "");

            if (json.contains("assets") && json["assets"].is_array()) {
                for (const auto& asset : json["assets"]) {
                    ReleaseAsset ra;
                    ra.name = asset.value("name", "");
                    ra.downloadUrl = asset.value("browser_download_url", "");
                    ra.size = asset.value("size", 0);
                    outInfo.assets.push_back(ra);
                }
            }

            Log::Info("Latest release: %s (%s) with %zu assets",
                      outInfo.tagName.c_str(), outInfo.name.c_str(), outInfo.assets.size());

        } catch (const nlohmann::json::exception& e) {
            Log::Error("Failed to parse GitHub API response: %s", e.what());
            return false;
        }

        // Select the platform-appropriate asset
        return SelectPlatformAsset(outInfo);
    }

    bool GitHubAPI::SelectPlatformAsset(ReleaseInfo& info) {
        std::string primaryTag = GetPlatformAssetTag();
        std::string fallbackTag = GetPlatformFallbackTag();

        Log::Info("Looking for asset matching: %s (fallback: %s)", primaryTag.c_str(), fallbackTag.c_str());

        // Helper to lowercase a string
        auto toLower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        };

        // Try exact platform+arch match first
        for (const auto& asset : info.assets) {
            std::string nameLower = toLower(asset.name);
            if (nameLower.find(toLower(primaryTag)) != std::string::npos) {
                info.platformAsset = asset;
                info.hasPlatformAsset = true;
                Log::Info("Found exact match: %s", asset.name.c_str());
                return true;
            }
        }

        // Try universal/generic platform match
        std::string universalTag = fallbackTag + "-universal";
        for (const auto& asset : info.assets) {
            std::string nameLower = toLower(asset.name);
            if (nameLower.find(toLower(universalTag)) != std::string::npos) {
                info.platformAsset = asset;
                info.hasPlatformAsset = true;
                Log::Info("Found universal match: %s", asset.name.c_str());
                return true;
            }
        }

        // Try generic platform fallback (e.g., just "macos" or "windows")
        for (const auto& asset : info.assets) {
            std::string nameLower = toLower(asset.name);
            if (nameLower.find(toLower(fallbackTag)) != std::string::npos) {
                info.platformAsset = asset;
                info.hasPlatformAsset = true;
                Log::Info("Found fallback match: %s", asset.name.c_str());
                return true;
            }
        }

        // Last resort: if there's only one zip asset, use it
        for (const auto& asset : info.assets) {
            std::string nameLower = toLower(asset.name);
            if (nameLower.find(".zip") != std::string::npos) {
                info.platformAsset = asset;
                info.hasPlatformAsset = true;
                Log::Warning("No platform-specific asset found, using: %s", asset.name.c_str());
                return true;
            }
        }

        Log::Error("No suitable asset found for platform: %s", primaryTag.c_str());
        info.hasPlatformAsset = false;
        return false;
    }

} // namespace Launcher
