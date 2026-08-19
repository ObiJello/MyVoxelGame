// File: src/client/network/FriendsClient.hpp
//
// Persistent NDJSON connection to the ObeyCraft friends service (see
// tools/friends_server/friends_service.py). One connection per process,
// alive across the title phase and every world session — which is why it
// owns a PRIVATE io_context + thread instead of reusing the per-session
// Client::NetworkIOService.
//
// Protocol: newline-delimited JSON over TCP. After connect we send
// {"op":"hello","token":...} once; the session is then authenticated and
// ops don't re-send the token. The server pushes:
//   {"event":"roster", friends/incoming/outgoing}  — full snapshots
//   {"event":"invite", from/world/host/port}       — friend invited us
// Requests carry a correlation "id" echoed on the response; friend-op
// targets travel in "friend" (NOT "id" — that's the correlation field).
//
// Threading: all socket work happens on the io thread. UI-facing state
// (roster snapshot, invite queue, join results, last error) is mutex-
// guarded and polled from the render/UI thread.
#pragma once

#include "common/network/AsioInclude.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Client {

    struct FriendPresence {
        enum class State { Offline, Menu, Playing, Hosting };
        State state = State::Offline;
        std::string world;   // world name (hosting) / server address (playing)
    };

    struct FriendEntry {
        int64_t id = 0;
        std::string name;
        FriendPresence presence;
    };

    struct FriendsRoster {
        std::vector<FriendEntry> friends;
        std::vector<FriendEntry> incoming;   // requests TO us (presence unused)
        std::vector<FriendEntry> outgoing;   // requests FROM us (presence unused)
        // Bumped on every snapshot swap; UI rebuilds when it changes.
        uint64_t generation = 0;
    };

    struct FriendInvite {
        int64_t fromId = 0;
        std::string fromName;
        std::string world;
        // No address: accepting an invite goes through join_info like any
        // other join, so direct-vs-relay is decided at click time.
    };

    struct JoinInfoResult {
        bool ok = false;
        // Direct: dial host:port. Relay: dial the friends service instead and
        // present `ticket` before the game handshake (the service splices us
        // to the host, who dialed out to meet us).
        bool relay = false;
        std::string host;
        uint16_t port = 0;
        std::string ticket;
        std::string world;
        std::string error;   // "not_hosting", "not_friends", "network"
    };

    class FriendsClient {
    public:
        FriendsClient() = default;
        ~FriendsClient();

        FriendsClient(const FriendsClient&) = delete;
        FriendsClient& operator=(const FriendsClient&) = delete;

        // Spins up the io thread and starts the connect/reconnect loop.
        void Start(std::string host, uint16_t port,
                   std::string token, int64_t accountId);
        void Stop();

        bool IsConnected() const { return m_connected.load(); }

        // Our own last-reported presence — UI-side mirror so the Friends
        // screen can gate "Invite" on "am I hosting?" without asking the
        // io thread.
        FriendPresence::State CurrentPresence() const {
            return static_cast<FriendPresence::State>(m_uiPresence.load());
        }

        // Snapshot copy for the UI. Compare `.generation` to skip rebuilds.
        FriendsRoster GetRoster() const;

        // Invites received since the last drain (PlatformMain consumes each
        // frame for the chat notification; FriendsScreen reads the cached
        // latest invite separately via LatestInvite()).
        std::vector<FriendInvite> ConsumeInvites();
        // Most recent invite, kept until joined/superseded — the Friends
        // screen renders it as a joinable banner row.
        bool LatestInvite(FriendInvite& out) const;
        void ClearLatestInvite();

        // ── Fire-and-forget ops (posted to the io thread) ──────────────
        void SendFriendRequest(const std::string& name);
        void AcceptRequest(int64_t friendId);
        void DeclineRequest(int64_t friendId);
        void RemoveFriend(int64_t friendId);
        // `externalIp` is the WAN address UPnP reported (empty if none) —
        // the service needs it when we share a LAN with it, since the
        // address it observes would then be a private one.
        void SetPresence(FriendPresence::State state,
                         const std::string& world, uint16_t hostPort,
                         const std::string& externalIp = "");
        void SendInvite(int64_t friendId);

        // Where this client is talking to — the joiner dials the same
        // address for relayed joins.
        const std::string& ServiceHost() const { return m_host; }
        uint16_t ServicePort() const { return m_port; }

        // Called (on the io thread) with an already-connected, relay-attached
        // socket that the integrated server should adopt as an inbound
        // player connection. PlatformMain wires this to
        // NetworkServer::AdoptConnection.
        using RelaySocketHandler =
            std::function<void(net::ip::tcp::socket::native_handle_type)>;
        void SetRelaySocketHandler(RelaySocketHandler handler) {
            m_relayHandler = std::move(handler);
        }

        // Join lookup: result is queued and delivered on the next
        // PollJoinResult call from the UI thread (no cross-thread callback
        // into UI code).
        void RequestJoinInfo(int64_t friendId);
        bool PollJoinResult(JoinInfoResult& out);

        // Last human-relevant op failure ("Friend is not hosting", …),
        // cleared on read. For the FriendsScreen status line.
        std::string ConsumeLastError();

    private:
        // io-thread internals
        void RunIOThread();
        void ScheduleConnect(int attempt);
        void DoConnect();
        void OnConnected();
        void ReadLoop();
        void HandleLine(const std::string& line);
        void SendJson(const std::string& serialized);
        void SendOp(const char* op, int64_t friendId);
        void ResendPresence();
        // Host side of a relayed join: dial the service, attach with the
        // ticket, hand the live socket to the integrated server.
        void DialRelay(const std::string& ticket);
        void StartPingTimer();
        void FailConnection();

        // Try the host address join_info handed us, then publish the result.
        //
        // The SERVICE cannot decide direct-vs-relay: when it shares a network
        // with the host, its reachability test is a NAT hairpin against the
        // host's own public IP, which many routers refuse for a UPnP-created
        // mapping — so it reported "unreachable" for hosts that were perfectly
        // reachable from the outside, and relayed everyone. The joiner is the
        // only party who can answer "can I reach this host", so it asks
        // directly. Falls back by re-requesting join_info with force_relay.
        void ProbeDirectThenPublish(JoinInfoResult direct, int64_t friendId);
        void PublishJoinResult(JoinInfoResult result);

        // ── io-thread-only state ───────────────────────────────────────
        net::io_context m_ioContext;
        using WorkGuard = net::executor_work_guard<net::io_context::executor_type>;
        std::unique_ptr<WorkGuard> m_workGuard;
        std::unique_ptr<std::thread> m_ioThread;
        std::unique_ptr<net::ip::tcp::socket> m_socket;
        std::unique_ptr<net::steady_timer> m_timer;      // reconnect backoff
        std::unique_ptr<net::steady_timer> m_pingTimer;
        net::streambuf m_readBuffer;
        int m_nextRequestId = 1;
        int m_reconnectAttempt = 0;
        bool m_helloAccepted = false;

        // Config (set once in Start before the io thread runs)
        std::string m_host;
        uint16_t m_port = 0;
        std::string m_token;
        int64_t m_accountId = 0;

        // Presence cache — re-sent after every reconnect.
        FriendPresence::State m_presenceState = FriendPresence::State::Menu;
        std::string m_presenceWorld;
        uint16_t m_presencePort = 0;
        std::string m_presenceExternalIp;

        RelaySocketHandler m_relayHandler;

        // ── Shared state (m_mutex) ─────────────────────────────────────
        mutable std::mutex m_mutex;
        FriendsRoster m_roster;
        std::vector<FriendInvite> m_pendingInvites;
        FriendInvite m_latestInvite;
        bool m_hasLatestInvite = false;
        std::vector<JoinInfoResult> m_joinResults;
        std::string m_lastError;
        int m_pendingJoinRequestId = 0;   // correlation id of in-flight join_info
        // Who we are joining, kept so a failed direct probe can re-ask with
        // force_relay. 0 = the in-flight request is already the relay retry.
        int64_t m_pendingJoinFriendId = 0;

        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_running{false};
        std::atomic<int> m_uiPresence{static_cast<int>(FriendPresence::State::Menu)};
    };

    // Global instance — null when running as a guest (no --session).
    extern std::unique_ptr<FriendsClient> g_friendsClient;

} // namespace Client
