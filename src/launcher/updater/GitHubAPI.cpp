// File: src/launcher/updater/GitHubAPI.cpp
#include "GitHubAPI.hpp"
#include "VersionInfo.hpp"
#include "HttpSession.hpp"
#include "launcher/LauncherConfig.hpp"
#include "common/core/Log.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace Launcher {

    GitHubAPI::GitHubAPI(const std::string& owner, const std::string& repo)
        : m_owner(owner), m_repo(repo) {}

    // GitHub's Link header looks like:
    //   Link: <https://api.github.com/.../releases?per_page=100&page=2>; rel="next",
    //         <https://api.github.com/.../releases?per_page=100&page=3>; rel="last"
    int GitHubAPI::ParseLastPage(const std::string& headerBlock) {
        std::string lower = headerBlock;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        const size_t link = lower.find("\nlink:");
        if (link == std::string::npos) return 1;
        size_t lineEnd = lower.find('\n', link + 1);
        if (lineEnd == std::string::npos) lineEnd = lower.size();
        const size_t rel = lower.find("rel=\"last\"", link);
        if (rel == std::string::npos || rel > lineEnd) return 1;
        const size_t open = lower.rfind('<', rel);
        const size_t close = lower.find('>', open);
        if (open == std::string::npos || open < link || close == std::string::npos || close > rel) return 1;
        const std::string url = lower.substr(open + 1, close - open - 1);
        // "page=" as its own parameter, not the tail of "per_page=".
        for (const char* key : {"?page=", "&page="}) {
            const size_t at = url.find(key);
            if (at != std::string::npos) return std::max(1, std::atoi(url.c_str() + at + 6));
        }
        return 1;
    }

    // The release list is newest-first, but the highest version of each tag
    // family is semver-picked across every page (FetchLatest*), so all pages are
    // read: the repo holds launcher and per-platform game releases, and the one
    // wanted can sit on page 2+.
    bool GitHubAPI::FetchAllReleases(std::vector<nlohmann::json>& outReleases) {
        if (m_releasesFetched) {
            outReleases = m_releases;
            return m_releasesOk;
        }
        m_releasesFetched = true;
        constexpr int MAX_PAGES = 10;
        const auto start = std::chrono::steady_clock::now();

        const std::string base = std::string(GitHubAPIBase) + "/repos/" + m_owner + "/" + m_repo
                               + "/releases?per_page=100";
        auto pageRequest = [&](int page) {
            Http::Request r;
            r.url = base + "&page=" + std::to_string(page);
            r.headers = {"Accept: application/vnd.github.v3+json"};
            r.acceptCompressed = true;
            return r;
        };

        Http::Session session;
        std::vector<Http::Request> pages;
        pages.push_back(pageRequest(1));
        pages[0].captureHeaders = true;
        session.Fetch({&pages[0]});
        if (!pages[0].Ok()) {
            Log::Error("Release list request failed: HTTP %ld %s", pages[0].status, pages[0].error.c_str());
            return false;
        }
        const int lastPage = std::min(ParseLastPage(pages[0].responseHeaders), MAX_PAGES);
        for (int page = 2; page <= lastPage; ++page) pages.push_back(pageRequest(page));
        if (pages.size() > 1) {
            std::vector<Http::Request*> rest;
            for (size_t i = 1; i < pages.size(); ++i) rest.push_back(&pages[i]);
            session.Fetch(rest);
        }

        for (size_t i = 0; i < pages.size(); ++i) {
            if (!pages[i].Ok()) continue;   // a missing page: semver-pick across the rest
            try {
                auto json = nlohmann::json::parse(pages[i].body.begin(), pages[i].body.end());
                if (!json.is_array()) {
                    Log::Error("Expected array of releases on page %zu", i + 1);
                    continue;
                }
                for (auto& r : json) m_releases.push_back(std::move(r));
            } catch (const nlohmann::json::exception& e) {
                Log::Error("Failed to parse releases JSON on page %zu: %s", i + 1, e.what());
            }
        }

        m_releasesOk = !m_releases.empty();
        Log::Info("Fetched %zu releases across %zu pages in %.0f ms", m_releases.size(), pages.size(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        outReleases = m_releases;
        return m_releasesOk;
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
        // Find the highest-versioned GAME release for this platform across the
        // ENTIRE release history (paginated). Single-page fetching used to fail
        // because the repo accumulates many launcher and per-platform game
        // releases — the platform-specific game release the user wanted could
        // be on page 2+ by GitHub's `created_at` ordering, and was simply missed.
        Log::Info("Checking for game updates...");

        std::vector<nlohmann::json> releases;
        if (!FetchAllReleases(releases)) return false;

        // Accept both new platform-specific prefix (e.g. "game-mac-v") and old format ("v")
        const std::string gamePrefix(GameReleaseTagPrefix);
        const std::string oldGamePrefix = "v";
        Version bestVersion;
        nlohmann::json bestRelease;
        bool found = false;

        for (const auto& release : releases) {
            std::string tag = release.value("tag_name", "");

            std::string matchedPrefix;
            if (tag.find(gamePrefix) == 0) {
                matchedPrefix = gamePrefix;
            } else if (tag.find(oldGamePrefix) == 0 && tag.find("launcher") == std::string::npos) {
                // Old format "v0.1.X" — but skip anything with "launcher" in it
                matchedPrefix = oldGamePrefix;
            } else {
                continue;
            }

            if (release.value("draft", false)) continue;
            if (release.value("prerelease", false)) continue;

            std::string versionStr = tag.substr(matchedPrefix.length());
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
        return SelectPlatformAsset(outInfo);
    }

    bool GitHubAPI::FetchLatestLauncherRelease(ReleaseInfo& outInfo) {
        // Same pagination strategy as FetchLatestRelease — paginate the entire
        // release history so we always find the highest-versioned platform-specific
        // launcher release, regardless of how many other releases sit ahead of it
        // in GitHub's `created_at` order.
        Log::Info("Checking for launcher updates...");

        std::vector<nlohmann::json> releases;
        if (!FetchAllReleases(releases)) return false;

        // Accept both new platform-specific prefix (e.g. "launcher-mac-v") and old format ("launcher-v")
        const std::string prefix(LauncherReleaseTagPrefix);
        const std::string oldPrefix = "launcher-v";
        Version bestVersion;
        nlohmann::json bestRelease;
        bool found = false;

        for (const auto& release : releases) {
            std::string tag = release.value("tag_name", "");

            std::string matchedPrefix;
            if (tag.find(prefix) == 0) {
                matchedPrefix = prefix;
            } else if (tag.find(oldPrefix) == 0) {
                matchedPrefix = oldPrefix;
            } else {
                continue;
            }

            std::string versionStr = tag.substr(matchedPrefix.length());
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
        auto endsWith = [](const std::string& s, const char* suffix) {
            const size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        };

        // The release's binary patch, if it has one. Its name carries no platform
        // tag (the release itself is per-platform), so it never matches below.
        info.hasPatchAsset = false;
        for (const auto& asset : info.assets) {
            if (endsWith(toLower(asset.name), ".obpatch")) {
                info.patchAsset = asset;
                info.hasPatchAsset = true;
                break;
            }
        }
        // Only zips are installable; anything else on a release is not the game.
        std::vector<ReleaseAsset> zips;
        for (const auto& asset : info.assets) {
            if (endsWith(toLower(asset.name), ".zip")) zips.push_back(asset);
        }

        // Try exact platform+arch match
        for (const auto& asset : zips) {
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
        for (const auto& asset : zips) {
            std::string nameLower = toLower(asset.name);
            if (nameLower.find(toLower(universalTag)) != std::string::npos) {
                info.platformAsset = asset;
                info.hasPlatformAsset = true;
                Log::Info("Found universal match: %s", asset.name.c_str());
                return true;
            }
        }

        // Try generic platform fallback
        for (const auto& asset : zips) {
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
