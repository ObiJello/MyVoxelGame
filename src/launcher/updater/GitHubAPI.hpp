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

        // Binary patches from the previous release's files (*.obpatch,
        // BinaryPatch). Named without a platform tag on purpose: launchers before
        // delta updates match assets by tag substring and must never pick it.
        ReleaseAsset patchAsset;
        bool hasPatchAsset = false;
    };

    // One instance answers both the launcher and the game check from a single
    // fetch of the release list (the first call fetches, the second reuses it).
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

        // Every release, newest pages first: page 1, then pages 2..last (from its
        // Link header) side by side on the same connection. Cached after the
        // first call. MAX_PAGES caps it at 1000 releases. True if any page came
        // back; the callers semver-pick across whatever was fetched.
        bool FetchAllReleases(std::vector<nlohmann::json>& outReleases);
        std::vector<nlohmann::json> m_releases;
        bool m_releasesFetched = false;
        bool m_releasesOk = false;

        // The page number in the Link header's rel="last" URL; 1 when there is none.
        static int ParseLastPage(const std::string& headerBlock);

        // Parse a single release JSON object into ReleaseInfo
        bool ParseRelease(const nlohmann::json& json, ReleaseInfo& outInfo);

        // Select the best asset for the current platform
        bool SelectPlatformAsset(ReleaseInfo& info);
    };

} // namespace Launcher
