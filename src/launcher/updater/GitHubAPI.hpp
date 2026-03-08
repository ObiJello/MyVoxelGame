// File: src/launcher/updater/GitHubAPI.hpp
#pragma once

#include <string>
#include <vector>

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

        // Fetch the latest release info. Returns true on success.
        bool FetchLatestRelease(ReleaseInfo& outInfo);

    private:
        std::string m_owner;
        std::string m_repo;

        // Select the best asset for the current platform
        bool SelectPlatformAsset(ReleaseInfo& info);
    };

} // namespace Launcher
