// File: src/launcher/net/FriendsServiceClient.hpp
//
// Blocking HTTP/JSON client for the ObeyCraft friends service (accounts,
// name availability, rename). One POST /api per call — the request pattern
// mirrors GitHubAPI::HttpGet with POSTFIELDS added.
//
// IMPORTANT: every method blocks on network I/O. Call ONLY from detached
// worker threads (the launcher's established background pattern), never
// from the ImGui render thread.
#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace Launcher {

    class FriendsServiceClient {
    public:
        FriendsServiceClient(std::string host, uint16_t port);

        struct Result {
            bool ok = false;
            // On failure: the service's error code ("name_taken",
            // "bad_credentials", …) or "network" when the request itself
            // failed. Empty on success.
            std::string error;
            nlohmann::json body;   // full response object
        };

        // Raw op call: POSTs the JSON request, parses the JSON response.
        Result Call(const nlohmann::json& request);

        // ── Typed wrappers ─────────────────────────────────────────────
        Result Signup(const std::string& name, const std::string& password);
        Result Login(const std::string& name, const std::string& password);
        Result Logout(const std::string& token);
        // `token` optional — with it, your own name reports "yours".
        Result CheckName(const std::string& name, const std::string& token);
        Result Rename(const std::string& token, const std::string& newName);

    private:
        std::string m_url;   // http://host:port/api
    };

} // namespace Launcher
