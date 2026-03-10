// File: src/launcher/updater/GitHubAPI.cpp
#include "GitHubAPI.hpp"
#include "VersionInfo.hpp"
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

    bool GitHubAPI::HttpGet(const std::string& url, std::string& outResponse) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            Log::Error("Failed to initialize curl");
            return false;
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/vnd.github.v3+json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outResponse);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            Log::Error("HTTP request failed: %s", curl_easy_strerror(res));
            return false;
        }

        if (httpCode != 200) {
            Log::Error("HTTP %ld from: %s", httpCode, url.c_str());
            return false;
        }

        return true;
    }

    bool GitHubAPI::ParseRelease(const nlohmann::json& json, ReleaseInfo& outInfo) {
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
        return true;
    }

    bool GitHubAPI::FetchLatestRelease(ReleaseInfo& outInfo) {
        // Query all releases and find the highest-versioned GAME release for this platform
        std::string url = std::string(GitHubAPIBase) + "/repos/" + m_owner + "/" + m_repo + "/releases?per_page=30";
        Log::Info("Checking for game updates...");

        std::string response;
        if (!HttpGet(url, response)) return false;

        try {
            auto releases = nlohmann::json::parse(response);
            if (!releases.is_array()) {
                Log::Error("Expected array of releases");
                return false;
            }

            std::string gamePrefix(GameReleaseTagPrefix);
            Version bestVersion;
            nlohmann::json bestRelease;
            bool found = false;

            for (const auto& release : releases) {
                std::string tag = release.value("tag_name", "");
                if (tag.find(gamePrefix) != 0) continue;
                if (release.value("draft", false)) continue;
                if (release.value("prerelease", false)) continue;

                std::string versionStr = tag.substr(gamePrefix.length());
                Version v = Version::Parse(versionStr);

                if (v.IsValid() && (!found || v > bestVersion)) {
                    bestVersion = v;
                    bestRelease = release;
                    found = true;
                }
            }

            if (!found) {
                Log::Info("No game releases found for this platform");
                return false;
            }

            ParseRelease(bestRelease, outInfo);
            Log::Info("Latest game release: %s (%s) with %zu assets",
                      outInfo.tagName.c_str(), outInfo.name.c_str(), outInfo.assets.size());
            SelectPlatformAsset(outInfo);
            return true;

        } catch (const nlohmann::json::exception& e) {
            Log::Error("Failed to parse releases JSON: %s", e.what());
            return false;
        }
    }

    bool GitHubAPI::FetchLatestLauncherRelease(ReleaseInfo& outInfo) {
        // Fetch all releases (up to 30 per page, which is plenty)
        std::string url = std::string(GitHubAPIBase) + "/repos/" + m_owner + "/" + m_repo + "/releases?per_page=30";
        Log::Info("Checking for launcher updates...");

        std::string response;
        if (!HttpGet(url, response)) return false;

        try {
            auto releases = nlohmann::json::parse(response);
            if (!releases.is_array()) {
                Log::Error("Expected array of releases");
                return false;
            }

            std::string prefix(LauncherReleaseTagPrefix);
            Version bestVersion;
            nlohmann::json bestRelease;
            bool found = false;

            for (const auto& release : releases) {
                std::string tag = release.value("tag_name", "");
                // Only consider releases tagged with "launcher-v"
                if (tag.find(prefix) != 0) continue;

                // Extract version from tag (strip "launcher-v" prefix)
                std::string versionStr = tag.substr(prefix.length());
                Version v = Version::Parse(versionStr);

                if (v.IsValid() && (!found || v > bestVersion)) {
                    bestVersion = v;
                    bestRelease = release;
                    found = true;
                }
            }

            if (!found) {
                Log::Info("No launcher releases found");
                return false;
            }

            ParseRelease(bestRelease, outInfo);
            Log::Info("Latest launcher release: %s with %zu assets",
                      outInfo.tagName.c_str(), outInfo.assets.size());

        } catch (const nlohmann::json::exception& e) {
            Log::Error("Failed to parse releases JSON: %s", e.what());
            return false;
        }

        return SelectPlatformAsset(outInfo);
    }

    bool GitHubAPI::SelectPlatformAsset(ReleaseInfo& info) {
        std::string primaryTag = GetPlatformAssetTag();
        std::string fallbackTag = GetPlatformFallbackTag();

        Log::Info("Looking for asset matching: %s (fallback: %s)", primaryTag.c_str(), fallbackTag.c_str());

        auto toLower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        };

        // Try exact platform+arch match
        for (const auto& asset : info.assets) {
            std::string nameLower = toLower(asset.name);
            if (nameLower.find(toLower(primaryTag)) != std::string::npos) {
                info.platformAsset = asset;
                info.hasPlatformAsset = true;
                Log::Info("Found exact match: %s", asset.name.c_str());
                return true;
            }
        }

        // Try universal
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

        // Try generic platform fallback
        for (const auto& asset : info.assets) {
            std::string nameLower = toLower(asset.name);
            if (nameLower.find(toLower(fallbackTag)) != std::string::npos) {
                info.platformAsset = asset;
                info.hasPlatformAsset = true;
                Log::Info("Found fallback match: %s", asset.name.c_str());
                return true;
            }
        }

        Log::Info("No asset found for platform: %s - skipping update", primaryTag.c_str());
        info.hasPlatformAsset = false;
        return false;
    }

} // namespace Launcher
