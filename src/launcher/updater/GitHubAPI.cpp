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

    bool GitHubAPI::HttpGetWithLink(const std::string& url,
                                    std::string& outBody,
                                    std::string& outNextUrl) {
        outNextUrl.clear();
        CURL* curl = curl_easy_init();
        if (!curl) {
            Log::Error("Failed to initialize curl");
            return false;
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/vnd.github.v3+json");

        std::string headerBuf;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBody);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerBuf);
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

        outNextUrl = ParseLinkNext(headerBuf);
        return true;
    }

    // GitHub Link header looks like:
    //   Link: <https://api.github.com/.../releases?per_page=100&page=2>; rel="next",
    //         <https://api.github.com/.../releases?per_page=100&page=4>; rel="last"
    // Find the URL whose rel parameter is "next" (case-insensitive).
    std::string GitHubAPI::ParseLinkNext(const std::string& headerBlock) {
        // Locate the "Link:" header line (case-insensitive).
        auto toLowerCopy = [](const std::string& s) {
            std::string out = s;
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return out;
        };
        const std::string lower = toLowerCopy(headerBlock);
        size_t lpos = lower.find("link:");
        if (lpos == std::string::npos) return "";

        size_t lineEnd = headerBlock.find("\r\n", lpos);
        if (lineEnd == std::string::npos) lineEnd = headerBlock.size();
        const std::string line = headerBlock.substr(lpos, lineEnd - lpos);

        // Each comma-separated piece: `<URL>; rel="something"`. Find the one
        // tagged rel="next" and pull the URL out of its angle brackets.
        size_t pos = 0;
        while (pos < line.size()) {
            size_t lt = line.find('<', pos);
            if (lt == std::string::npos) break;
            size_t gt = line.find('>', lt + 1);
            if (gt == std::string::npos) break;
            size_t semi = line.find(';', gt);
            size_t comma = line.find(',', gt);
            // `comma` is either npos (no further entries) or a valid position
            // within `line`, so it's always <= line.size() — no clamp needed.
            // Using std::min here would collide with the Windows `min` macro
            // unless we wrap it in extra parens; just inline the conditional.
            size_t segEnd = (comma == std::string::npos) ? line.size() : comma;
            const std::string segParams = line.substr(gt + 1,
                                                      (segEnd > gt + 1) ? (segEnd - gt - 1) : 0);
            const std::string segLower = toLowerCopy(segParams);
            if (segLower.find("rel=\"next\"") != std::string::npos) {
                return line.substr(lt + 1, gt - lt - 1);
            }
            pos = (comma == std::string::npos) ? line.size() : (comma + 1);
            (void)semi; // unused — segParams already starts after '>'
        }
        return "";
    }

    // Walk every page until exhausted or MAX_PAGES hit. Each page can hold up
    // to 100 releases (GitHub's max), so MAX_PAGES=10 covers 1000 releases —
    // many years of active development. The semantic-version comparison in the
    // caller picks the highest match across the union of all pages, so the
    // algorithm is correct regardless of GitHub's `created_at` ordering.
    bool GitHubAPI::FetchAllReleases(std::vector<nlohmann::json>& outReleases) {
        outReleases.clear();
        constexpr int MAX_PAGES = 10;

        std::string url = std::string(GitHubAPIBase) + "/repos/" + m_owner + "/" + m_repo
                        + "/releases?per_page=100";

        for (int page = 0; page < MAX_PAGES; ++page) {
            std::string body;
            std::string nextUrl;
            if (!HttpGetWithLink(url, body, nextUrl)) {
                // Network failure: return what we already have. Empty result =>
                // caller treats as "no releases found"; partial result is still
                // usable since we semver-pick the best match downstream.
                return !outReleases.empty();
            }
            try {
                auto json = nlohmann::json::parse(body);
                if (!json.is_array()) {
                    Log::Error("Expected array of releases on page %d", page + 1);
                    break;
                }
                for (const auto& r : json) outReleases.push_back(r);
            } catch (const nlohmann::json::exception& e) {
                Log::Error("Failed to parse releases JSON on page %d: %s", page + 1, e.what());
                break;
            }
            if (nextUrl.empty()) break; // last page
            url = nextUrl;
        }

        Log::Info("Fetched %zu releases across pagination", outReleases.size());
        return !outReleases.empty();
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
