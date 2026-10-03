// File: src/client/sync/FileSyncService.hpp
//
// Driver for the optional chat.db migration feature. Lives client-side
// because its transport reaches the other Mac the SAME way the game reaches a
// hosted friend: through the ObeyCraft friends service, UPnP-direct with a
// relay fallback. It reuses the service's token auth and relay splice and the
// UPnP port mapper, but is deliberately ISOLATED — it opens its OWN dedicated
// service connection and never touches the game's FriendsClient, presence, or
// relay handler. The rsync delta engine and all safety behaviour live in
// common/sync/FileDeltaSync and are used unchanged.
//
// Isolation contract (see FileSyncConfig.hpp and docs/file-sync.md):
//   • If kFileSyncSourceUsername is empty, StartIfConfigured() returns
//     immediately having touched nothing — no thread, no sockets, no cost.
//   • When on, ALL work runs on its own background thread(s), wrapped so any
//     exception/error/timeout is caught and logged; the game proceeds exactly
//     as normal.
//   • It never touches game state, never blocks the title screen, and
//     RequestStop() joins with a short timeout then detaches so shutdown is
//     never held up.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace Sync {

    class FileSyncService {
    public:
        static FileSyncService& Instance();

        // Config needed to reach the friends service (the same values the game
        // used for its own FriendsClient). An empty `token` means the player
        // is a guest — the feature cannot use the service, so it logs and does
        // nothing (retried on a future launch once signed in).
        struct Params {
            std::string localPlayerName;
            std::string gameDir;
            std::string serviceHost;
            uint16_t    servicePort = 0;
            std::string token;
            int64_t     accountId = 0;
        };

        // Start once per process. No-op if kFileSyncSourceUsername is empty or
        // if already started. Safe to call from the title screen.
        void StartIfConfigured(const Params& params);

        // Signal the worker to stop, join with a short timeout, else detach.
        // Idempotent; safe on any shutdown path.
        void RequestStop();

        // A short, human-readable status line for optional display/logging.
        std::string StatusLine();

        // Set the status line; public so the detached helper threads can
        // report progress (same reason Stopping() is public).
        void SetStatus(const std::string& s);

        // Observed by the background thread(s); public so detached helper
        // threads can poll it.
        bool Stopping() const { return m_stop.load(); }

    private:
        FileSyncService() = default;
        ~FileSyncService();
        FileSyncService(const FileSyncService&) = delete;
        FileSyncService& operator=(const FileSyncService&) = delete;

        void Worker(Params params);

        std::thread m_thread;
        std::atomic<bool> m_started{false};
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_finished{false};
        std::mutex m_statusMutex;
        std::string m_status;
    };

} // namespace Sync
