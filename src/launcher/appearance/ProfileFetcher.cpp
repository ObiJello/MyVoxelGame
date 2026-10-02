// File: src/launcher/appearance/ProfileFetcher.cpp
#include "ProfileFetcher.hpp"
#include "SkinIO.hpp"
#include "launcher/updater/HttpSession.hpp"
#include "common/core/Log.hpp"
#include "common/entity/PlayerCapes.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace Launcher::Appearance {

    struct ProfileFetcher::Job {
        std::string username;
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        ProfileResult result;
    };

    namespace {

        // One GET. False with `error` set on a transport failure; an HTTP
        // status is the caller's to judge.
        bool Get(Http::Session& session, const std::string& url, Http::Request& r, std::string& error) {
            r = Http::Request{};
            r.url = url;
            r.headers.push_back("Accept: application/json");
            session.Fetch({ &r }, 1);
            if (!r.error.empty()) {
                error = "Could not reach " + url.substr(0, url.find('/', 8)) + " (" + r.error + ")";
                return false;
            }
            return true;
        }

        // texture URLs come back as http://; the server speaks TLS too.
        std::string Secure(std::string url) {
            if (url.rfind("http://", 0) == 0) url = "https://" + url.substr(7);
            return url;
        }

        bool DownloadImage(Http::Session& session, const std::string& url, Game::SkinImage& out,
                           std::string& error) {
            Http::Request r;
            r.url = Secure(url);
            session.Fetch({ &r }, 1);
            if (!r.Ok()) {
                error = r.error.empty() ? "texture server answered " + std::to_string(r.status) : r.error;
                return false;
            }
            if (!Game::DecodePng(r.body, out)) {
                error = "the texture is not a PNG";
                return false;
            }
            return true;
        }

        ProfileResult Run(const std::string& username) {
            ProfileResult res;
            Http::Session session;
            std::string error;

            // 1. name → uuid.
            std::string uuid, name;
            {
                Http::Request r;
                if (!Get(session, "https://api.mojang.com/users/profiles/minecraft/" + username, r, error)) {
                    res.error = error;
                    return res;
                }
                if (r.status != 200) {
                    // api.mojang.com has been retired in stages; the services
                    // host answers the same question.
                    Http::Request r2;
                    std::string error2;
                    if (Get(session, "https://api.minecraftservices.com/minecraft/profile/lookup/name/" + username,
                            r2, error2) && r2.status == 200) {
                        r = std::move(r2);
                    }
                }
                if (r.status == 204 || r.status == 404) {
                    res.error = "No Java profile is called \"" + username + "\"";
                    return res;
                }
                if (r.status == 429) {
                    res.error = "Mojang is rate limiting lookups; try again in a minute";
                    return res;
                }
                if (r.status != 200) {
                    res.error = "Profile lookup failed (HTTP " + std::to_string(r.status) + ")";
                    return res;
                }
                try {
                    const auto json = nlohmann::json::parse(r.body.begin(), r.body.end());
                    uuid = json.value("id", std::string());
                    name = json.value("name", username);
                } catch (...) {
                    res.error = "Unexpected answer from the profile lookup";
                    return res;
                }
                if (uuid.empty()) {
                    res.error = "No Java profile is called \"" + username + "\"";
                    return res;
                }
            }

            // 2. uuid → textures property.
            std::string skinUrl, capeUrl;
            bool slim = false;
            {
                Http::Request r;
                if (!Get(session, "https://sessionserver.mojang.com/session/minecraft/profile/" + uuid, r, error)) {
                    res.error = error;
                    return res;
                }
                if (r.status == 429) {
                    res.error = "Mojang is rate limiting lookups; try again in a minute";
                    return res;
                }
                if (r.status != 200) {
                    res.error = "Profile textures unavailable (HTTP " + std::to_string(r.status) + ")";
                    return res;
                }
                try {
                    const auto json = nlohmann::json::parse(r.body.begin(), r.body.end());
                    name = json.value("name", name);
                    std::string encoded;
                    if (json.contains("properties") && json["properties"].is_array()) {
                        for (const auto& p : json["properties"]) {
                            if (p.value("name", std::string()) == "textures") encoded = p.value("value", std::string());
                        }
                    }
                    if (!encoded.empty()) {
                        std::vector<uint8_t> decoded;
                        if (!Base64Decode(encoded, decoded)) throw std::runtime_error("base64");
                        const auto tex = nlohmann::json::parse(decoded.begin(), decoded.end());
                        if (tex.contains("textures") && tex["textures"].is_object()) {
                            const auto& t = tex["textures"];
                            if (t.contains("SKIN") && t["SKIN"].is_object()) {
                                skinUrl = t["SKIN"].value("url", std::string());
                                if (t["SKIN"].contains("metadata") && t["SKIN"]["metadata"].is_object()) {
                                    slim = t["SKIN"]["metadata"].value("model", std::string()) == "slim";
                                }
                            }
                            if (t.contains("CAPE") && t["CAPE"].is_object()) {
                                capeUrl = t["CAPE"].value("url", std::string());
                            }
                        }
                    }
                } catch (...) {
                    res.error = "Unexpected answer from the session server";
                    return res;
                }
            }

            res.uuid = uuid;
            res.name = name;
            res.model = slim ? Game::SkinModel::Slim : Game::SkinModel::Classic;

            // 3. The textures.
            if (!skinUrl.empty()) {
                Game::SkinImage skin;
                if (!DownloadImage(session, skinUrl, skin, error)) {
                    res.error = "Could not download the skin: " + error;
                    return res;
                }
                if (!NormalizeSkin(skin)) {
                    res.error = "The profile's skin is " + std::to_string(skin.width) + "x" +
                                std::to_string(skin.height) + ", not a Minecraft skin";
                    return res;
                }
                res.hasSkin = true;
                res.skin = std::move(skin);
            }
            if (!capeUrl.empty()) {
                Game::SkinImage cape;
                std::string capeError;
                if (DownloadImage(session, capeUrl, cape, capeError) && NormalizeCape(cape)) {
                    res.hasCape = true;
                    res.cape = std::move(cape);
                    if (const Game::CapeInfo* known = Game::FindCapeByUrl(capeUrl)) {
                        res.capeSlug = std::string(known->slug);
                    }
                } else {
                    // The skin still stands without its cape.
                    Log::Warning("[Appearance] %s's cape could not be loaded: %s",
                                 name.c_str(), capeError.c_str());
                }
            }
            res.ok = true;
            return res;
        }

    } // namespace

    ProfileFetcher::ProfileFetcher() = default;

    ProfileFetcher::~ProfileFetcher() {
        if (m_job) m_job->cancelled = true;
    }

    bool ProfileFetcher::IsValidName(const std::string& name) {
        if (name.empty() || name.size() > 16) return false;
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_';
            if (!ok) return false;
        }
        return true;
    }

    void ProfileFetcher::Start(const std::string& username) {
        if (m_job) m_job->cancelled = true;
        auto job = std::make_shared<Job>();
        job->username = username;
        m_job = job;
        m_pendingName = username;
        std::thread([job]() {
            ProfileResult result = Run(job->username);
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->result = std::move(result);
            }
            job->done = true;
        }).detach();
    }

    bool ProfileFetcher::Busy() const {
        return m_job && !m_job->done.load();
    }

    bool ProfileFetcher::Poll(ProfileResult& out) {
        if (!m_job || !m_job->done.load()) return false;
        {
            std::lock_guard<std::mutex> lock(m_job->mutex);
            out = std::move(m_job->result);
        }
        m_job.reset();
        m_pendingName.clear();
        return true;
    }

} // namespace Launcher::Appearance
