// File: src/launcher/updater/GitHubAPI.hpp
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Launcher {

    struct ReleaseAsset {
        std::string name;
        std::string downloadUrl;
        size_t size = 0;
    };

    struct ReleaseInfo {
        std::string tagName;
        std::string name;
        std::string body;         // changelog / release notes
        std::string publishedAt;
        std::vector<ReleaseAsset> assets;

        // Selected asset for this platform
        ReleaseAsset platformAsset;
        bool hasPlatformAsset = false;
    };

    class GitHubAPI {
    public:
        GitHubAPI(const std::string& owner, const std::string& repo);

        // Fetch the latest game release info (tags like v0.1.0). Returns true on success.
        bool FetchLatestRelease(ReleaseInfo& outInfo);

        // Fetch the latest launcher release info (tags like launcher-v1.0.0). Returns true on success.
        bool FetchLatestLauncherRelease(ReleaseInfo& outInfo);

    private:
        std::string m_owner;
        std::string m_repo;

        // Perform a GET request and return the response body
        bool HttpGet(const std::string& url, std::string& outResponse);

        // Parse a single release JSON object into ReleaseInfo
        bool ParseRelease(const nlohmann::json& json, ReleaseInfo& outInfo);

        // Select the best asset for the current platform
        bool SelectPlatformAsset(ReleaseInfo& info);
    };

} // namespace Launcher
