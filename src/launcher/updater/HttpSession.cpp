// File: src/launcher/updater/HttpSession.cpp
#include "launcher/updater/HttpSession.hpp"
#include "launcher/LauncherConfig.hpp"
#include "common/core/Log.hpp"

#include <curl/curl.h>
#include <string>

namespace Launcher::Http {

    namespace {

        struct Transfer {
            Request* request = nullptr;
            CURL* easy = nullptr;
            curl_slist* headers = nullptr;
            uint64_t* received = nullptr;   // the batch's running total (one thread)
        };

        size_t WriteBody(void* data, size_t size, size_t count, void* user) {
            auto* t = static_cast<Transfer*>(user);
            const size_t n = size * count;
            const auto* bytes = static_cast<const uint8_t*>(data);
            t->request->body.insert(t->request->body.end(), bytes, bytes + n);
            *t->received += n;
            return n;
        }

        size_t WriteHeader(void* data, size_t size, size_t count, void* user) {
            auto* t = static_cast<Transfer*>(user);
            const size_t n = size * count;
            if (t->request->captureHeaders) t->request->responseHeaders.append(static_cast<const char*>(data), n);
            return n;
        }

    } // namespace

    bool Request::Ok() const {
        if (!error.empty()) return false;
        if (!ranged) return status == 200;
        return status == 206 && body.size() == rangeLast - rangeFirst + 1;
    }

    Session::Session() {
        CURLM* multi = curl_multi_init();
        if (multi) {
            curl_multi_setopt(multi, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_MULTIPLEX));
        }
        m_multi = multi;
    }

    Session::~Session() {
        if (m_multi) curl_multi_cleanup(static_cast<CURLM*>(m_multi));
    }

    bool Session::Fetch(const std::vector<Request*>& requests, int maxParallel,
                        const std::function<void(Request&)>& onDone,
                        const std::function<void(uint64_t received)>& onProgress) {
        CURLM* multi = static_cast<CURLM*>(m_multi);
        if (!multi) {
            for (Request* r : requests) r->error = "curl unavailable";
            return false;
        }
        curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(maxParallel));
        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(maxParallel));

        uint64_t received = 0;
        std::vector<Transfer> transfers(requests.size());
        for (size_t i = 0; i < requests.size(); i++) {
            Transfer& t = transfers[i];
            Request& r = *requests[i];
            r.body.clear();
            r.responseHeaders.clear();
            r.error.clear();
            r.status = 0;
            if (r.ranged) r.body.reserve(static_cast<size_t>(r.rangeLast - r.rangeFirst + 1));

            t.request = &r;
            t.received = &received;
            t.easy = curl_easy_init();
            if (!t.easy) {
                r.error = "curl_easy_init failed";
                continue;
            }
            for (const std::string& h : r.headers) t.headers = curl_slist_append(t.headers, h.c_str());

            curl_easy_setopt(t.easy, CURLOPT_URL, r.url.c_str());
            curl_easy_setopt(t.easy, CURLOPT_USERAGENT, UserAgent);
            if (t.headers) curl_easy_setopt(t.easy, CURLOPT_HTTPHEADER, t.headers);
            curl_easy_setopt(t.easy, CURLOPT_WRITEFUNCTION, WriteBody);
            curl_easy_setopt(t.easy, CURLOPT_WRITEDATA, &t);
            curl_easy_setopt(t.easy, CURLOPT_HEADERFUNCTION, WriteHeader);
            curl_easy_setopt(t.easy, CURLOPT_HEADERDATA, &t);
            curl_easy_setopt(t.easy, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(t.easy, CURLOPT_MAXREDIRS, 10L);
            curl_easy_setopt(t.easy, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(t.easy, CURLOPT_CONNECTTIMEOUT, 15L);
            // A stalled transfer (under 1 KB/s for 30 s) fails; a slow but moving one does not.
            curl_easy_setopt(t.easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(t.easy, CURLOPT_LOW_SPEED_TIME, 30L);
            curl_easy_setopt(t.easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
            // Wait for an HTTP/2 connection to multiplex on rather than opening another.
            curl_easy_setopt(t.easy, CURLOPT_PIPEWAIT, 1L);
            curl_easy_setopt(t.easy, CURLOPT_PRIVATE, &t);
            if (r.acceptCompressed) curl_easy_setopt(t.easy, CURLOPT_ACCEPT_ENCODING, "");
            std::string range;
            if (r.ranged) {
                range = std::to_string(r.rangeFirst) + "-" + std::to_string(r.rangeLast);
                curl_easy_setopt(t.easy, CURLOPT_RANGE, range.c_str());   // copied by curl
            }
            curl_multi_add_handle(multi, t.easy);
        }

        int running = 0;
        do {
            CURLMcode mc = curl_multi_perform(multi, &running);
            if (mc != CURLM_OK) {
                Log::Error("[Http] curl_multi_perform: %s", curl_multi_strerror(mc));
                break;
            }
            int queued = 0;
            while (CURLMsg* msg = curl_multi_info_read(multi, &queued)) {
                if (msg->msg != CURLMSG_DONE) continue;
                Transfer* t = nullptr;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, reinterpret_cast<char**>(&t));
                Request& r = *t->request;
                if (msg->data.result != CURLE_OK) r.error = curl_easy_strerror(msg->data.result);
                curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &r.status);
                char* effective = nullptr;
                curl_easy_getinfo(t->easy, CURLINFO_EFFECTIVE_URL, &effective);
                if (effective) r.effectiveUrl = effective;
                curl_multi_remove_handle(multi, t->easy);
                curl_easy_cleanup(t->easy);
                t->easy = nullptr;
                curl_slist_free_all(t->headers);
                t->headers = nullptr;
                if (!r.Ok()) {
                    Log::Warning("[Http] %s%s%s: HTTP %ld, %zu bytes%s%s", r.url.c_str(), r.ranged ? " range " : "",
                                 r.ranged ? (std::to_string(r.rangeFirst) + "-" + std::to_string(r.rangeLast)).c_str() : "",
                                 r.status, r.body.size(), r.error.empty() ? "" : ", ", r.error.c_str());
                }
                if (onDone) onDone(r);
            }
            if (onProgress) onProgress(received);
            if (running) curl_multi_poll(multi, nullptr, 0, 100, nullptr);
        } while (running);

        // Anything left (a multi error above) is torn down here.
        bool allOk = true;
        for (Transfer& t : transfers) {
            if (t.easy) {
                curl_multi_remove_handle(multi, t.easy);
                curl_easy_cleanup(t.easy);
                curl_slist_free_all(t.headers);
                if (t.request->error.empty()) t.request->error = "transfer did not complete";
            }
            allOk = allOk && t.request->Ok();
        }
        return allOk;
    }

} // namespace Launcher::Http
