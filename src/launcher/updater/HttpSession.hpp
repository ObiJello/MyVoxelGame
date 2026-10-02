// File: src/launcher/updater/HttpSession.hpp
//
// Concurrent HTTP GETs over one curl multi handle. Requests to the same host
// share a connection (HTTP/2 streams where the server speaks it, otherwise up
// to `maxParallel` HTTP/1.1 connections), and the multi handle's connection
// cache outlives a batch, so a second Fetch on the same session skips the TCP
// and TLS handshakes. Used for the GitHub release list's pages and for the
// delta update's byte ranges of a release zip.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Launcher::Http {

    struct Request {
        std::string url;
        std::vector<std::string> headers;      // "Name: value"
        bool     ranged = false;               // Range: bytes=rangeFirst-rangeLast (inclusive)
        uint64_t rangeFirst = 0;
        uint64_t rangeLast = 0;
        bool     captureHeaders = false;       // keep the response headers in responseHeaders
        bool     acceptCompressed = false;     // let the server gzip the body (curl inflates it)

        // Results
        std::vector<uint8_t> body;
        std::string responseHeaders;
        std::string effectiveUrl;              // after redirects
        std::string error;                     // transport error, empty when the request completed
        long        status = 0;

        // 200 for a plain GET; for a ranged one, 206 with exactly the bytes asked for
        // (a server that ignores Range answers 200 with the whole file).
        bool Ok() const;
    };

    class Session {
    public:
        Session();
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Runs every request to completion. `onDone` runs on the calling thread as
        // each one finishes, so a caller can start on a response while the rest
        // are still arriving; `onProgress` runs there too, with the body bytes
        // received so far, a few times a second. True when every request is Ok().
        bool Fetch(const std::vector<Request*>& requests, int maxParallel = 6,
                   const std::function<void(Request&)>& onDone = {},
                   const std::function<void(uint64_t received)>& onProgress = {});

    private:
        void* m_multi = nullptr;   // CURLM*, kept out of the header
    };

} // namespace Launcher::Http
