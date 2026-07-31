// File: src/launcher/net/FriendsServiceClient.cpp
#include "FriendsServiceClient.hpp"
#include "launcher/LauncherConfig.hpp"
#include "common/core/Log.hpp"
#include <curl/curl.h>

namespace Launcher {

    namespace {
        size_t WriteCallback(void* contents, size_t size, size_t nmemb,
                             std::string* output) {
            const size_t totalSize = size * nmemb;
            output->append(static_cast<char*>(contents), totalSize);
            return totalSize;
        }
    }

    FriendsServiceClient::FriendsServiceClient(std::string host, uint16_t port)
        : m_url("http://" + std::move(host) + ":" + std::to_string(port) + "/api") {}

    FriendsServiceClient::Result FriendsServiceClient::Call(const nlohmann::json& request) {
        Result result;

        CURL* curl = curl_easy_init();
        if (!curl) {
            result.error = "network";
            return result;
        }

        const std::string payload = request.dump();
        std::string response;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        const CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            Log::Warning("[FriendsService] %s: %s",
                         request.value("op", "?").c_str(), curl_easy_strerror(res));
            result.error = "network";
            return result;
        }

        try {
            result.body = nlohmann::json::parse(response);
        } catch (const std::exception&) {
            Log::Warning("[FriendsService] bad response (HTTP %ld)", httpCode);
            result.error = "network";
            return result;
        }

        result.ok = result.body.value("ok", false);
        if (!result.ok) {
            result.error = result.body.value("error", "unknown");
        }
        return result;
    }

    FriendsServiceClient::Result FriendsServiceClient::Signup(
            const std::string& name, const std::string& password) {
        return Call({{"op", "signup"}, {"name", name}, {"password", password}});
    }

    FriendsServiceClient::Result FriendsServiceClient::Login(
            const std::string& name, const std::string& password) {
        return Call({{"op", "login"}, {"name", name}, {"password", password}});
    }

    FriendsServiceClient::Result FriendsServiceClient::Logout(const std::string& token) {
        return Call({{"op", "logout"}, {"token", token}});
    }

    FriendsServiceClient::Result FriendsServiceClient::CheckName(
            const std::string& name, const std::string& token) {
        nlohmann::json req{{"op", "check_name"}, {"name", name}};
        if (!token.empty()) req["token"] = token;
        return Call(req);
    }

    FriendsServiceClient::Result FriendsServiceClient::Rename(
            const std::string& token, const std::string& newName) {
        return Call({{"op", "rename"}, {"token", token}, {"name", newName}});
    }

} // namespace Launcher
