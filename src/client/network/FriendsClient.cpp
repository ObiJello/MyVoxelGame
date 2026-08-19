// File: src/client/network/FriendsClient.cpp
#include "FriendsClient.hpp"
#include "common/core/Log.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace Client {

    std::unique_ptr<FriendsClient> g_friendsClient;

    namespace {
        // Reconnect backoff ladder (seconds). Sticks at the last step.
        constexpr int kBackoff[] = {5, 15, 30};
        constexpr int kPingIntervalSeconds = 30;

        FriendPresence::State ParseState(const std::string& s) {
            if (s == "menu")    return FriendPresence::State::Menu;
            if (s == "playing") return FriendPresence::State::Playing;
            if (s == "hosting") return FriendPresence::State::Hosting;
            return FriendPresence::State::Offline;
        }

        const char* StateName(FriendPresence::State s) {
            switch (s) {
                case FriendPresence::State::Menu:    return "menu";
                case FriendPresence::State::Playing: return "playing";
                case FriendPresence::State::Hosting: return "hosting";
                default:                             return "menu";
            }
        }

        std::vector<FriendEntry> ParseEntryList(const nlohmann::json& arr) {
            std::vector<FriendEntry> out;
            if (!arr.is_array()) return out;
            for (const auto& e : arr) {
                FriendEntry entry;
                entry.id = e.value("id", static_cast<int64_t>(0));
                entry.name = e.value("name", "");
                if (e.contains("presence")) {
                    const auto& p = e["presence"];
                    entry.presence.state = ParseState(p.value("state", "offline"));
                    entry.presence.world = p.value("world", "");
                }
                out.push_back(std::move(entry));
            }
            return out;
        }
    } // namespace

    FriendsClient::~FriendsClient() { Stop(); }

    // ── lifecycle ───────────────────────────────────────────────────────────

    void FriendsClient::Start(std::string host, uint16_t port,
                              std::string token, int64_t accountId) {
        if (m_running.exchange(true)) return;
        m_host = std::move(host);
        m_port = port;
        m_token = std::move(token);
        m_accountId = accountId;

        m_workGuard = std::make_unique<WorkGuard>(m_ioContext.get_executor());
        m_ioThread = std::make_unique<std::thread>([this] { RunIOThread(); });
        net::post(m_ioContext, [this] { DoConnect(); });
        Log::Info("[Friends] client started (%s:%u, account %lld)",
                  m_host.c_str(), static_cast<unsigned>(m_port),
                  static_cast<long long>(m_accountId));
    }

    void FriendsClient::Stop() {
        if (!m_running.exchange(false)) return;
        net::post(m_ioContext, [this] {
            if (m_socket) {
                error_code ec;
                m_socket->close(ec);
            }
            if (m_timer) m_timer->cancel();
            if (m_pingTimer) m_pingTimer->cancel();
        });
        m_workGuard.reset();
        if (m_ioThread && m_ioThread->joinable()) m_ioThread->join();
        m_ioThread.reset();
        m_connected = false;
        Log::Info("[Friends] client stopped");
    }

    void FriendsClient::RunIOThread() {
        try {
            m_ioContext.run();
        } catch (const std::exception& e) {
            Log::Error("[Friends] io thread exception: %s", e.what());
        }
    }

    // ── connect / reconnect ─────────────────────────────────────────────────

    void FriendsClient::ScheduleConnect(int attempt) {
        if (!m_running) return;
        const int step = std::min(attempt,
            static_cast<int>(std::size(kBackoff)) - 1);
        const int delay = kBackoff[step];
        if (!m_timer) m_timer = std::make_unique<net::steady_timer>(m_ioContext);
        m_timer->expires_after(std::chrono::seconds(delay));
        m_timer->async_wait([this](const error_code& ec) {
            if (!ec && m_running) DoConnect();
        });
    }

    void FriendsClient::DoConnect() {
        m_helloAccepted = false;
        m_socket = std::make_unique<net::ip::tcp::socket>(m_ioContext);

        auto resolver = std::make_shared<net::ip::tcp::resolver>(m_ioContext);
        resolver->async_resolve(m_host, std::to_string(m_port),
            [this, resolver](const error_code& ec,
                             net::ip::tcp::resolver::results_type results) {
                if (ec || !m_running) {
                    if (m_running) FailConnection();
                    return;
                }
                net::async_connect(*m_socket, results,
                    [this](const error_code& ec2, const net::ip::tcp::endpoint&) {
                        if (ec2 || !m_running) {
                            if (m_running) FailConnection();
                            return;
                        }
                        OnConnected();
                    });
            });
    }

    void FriendsClient::OnConnected() {
        m_reconnectAttempt = 0;
        // Authenticate; the server replies then pushes the first roster.
        nlohmann::json hello{
            {"op", "hello"}, {"token", m_token}, {"proto", 1},
            {"id", m_nextRequestId++}};
        SendJson(hello.dump());
        ReadLoop();
        StartPingTimer();
    }

    void FriendsClient::FailConnection() {
        const bool wasConnected = m_connected.exchange(false);
        if (wasConnected) Log::Info("[Friends] disconnected — will reconnect");
        if (m_socket) {
            error_code ec;
            m_socket->close(ec);
        }
        ScheduleConnect(m_reconnectAttempt++);
    }

    // ── io ──────────────────────────────────────────────────────────────────

    void FriendsClient::SendJson(const std::string& serialized) {
        if (!m_socket || !m_socket->is_open()) return;
        auto data = std::make_shared<std::string>(serialized + "\n");
        net::async_write(*m_socket, net::buffer(*data),
            [this, data](const error_code& ec, std::size_t) {
                if (ec && m_running) FailConnection();
            });
    }

    void FriendsClient::ReadLoop() {
        net::async_read_until(*m_socket, m_readBuffer, '\n',
            [this](const error_code& ec, std::size_t) {
                if (ec || !m_running) {
                    if (m_running) FailConnection();
                    return;
                }
                std::istream is(&m_readBuffer);
                std::string line;
                std::getline(is, line);
                if (!line.empty()) HandleLine(line);
                if (m_running) ReadLoop();
            });
    }

    void FriendsClient::StartPingTimer() {
        if (!m_pingTimer)
            m_pingTimer = std::make_unique<net::steady_timer>(m_ioContext);
        m_pingTimer->expires_after(std::chrono::seconds(kPingIntervalSeconds));
        m_pingTimer->async_wait([this](const error_code& ec) {
            if (ec || !m_running || !m_connected) return;
            nlohmann::json ping{{"op", "ping"}, {"id", m_nextRequestId++}};
            SendJson(ping.dump());
            StartPingTimer();
        });
    }

    void FriendsClient::HandleLine(const std::string& line) {
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(line);
        } catch (const std::exception&) {
            return;
        }

        // Server-pushed events.
        const std::string event = msg.value("event", "");
        if (event == "roster") {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_roster.friends  = ParseEntryList(msg.value("friends", nlohmann::json::array()));
            m_roster.incoming = ParseEntryList(msg.value("incoming", nlohmann::json::array()));
            m_roster.outgoing = ParseEntryList(msg.value("outgoing", nlohmann::json::array()));
            ++m_roster.generation;
            return;
        }
        if (event == "invite") {
            // No address in the invite on purpose — the invitee resolves it
            // via join_info at click time, so direct-vs-relay is decided
            // fresh (and the host's IP isn't broadcast around).
            FriendInvite invite;
            invite.fromId = msg["from"].value("id", static_cast<int64_t>(0));
            invite.fromName = msg["from"].value("name", "");
            invite.world = msg.value("world", "");
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingInvites.push_back(invite);
            m_latestInvite = invite;
            m_hasLatestInvite = true;
            return;
        }
        if (event == "relay_open") {
            // A friend is joining us but can't reach us directly: dial out
            // to the relay and let it splice us together.
            DialRelay(msg.value("ticket", ""));
            return;
        }

        // Responses.
        if (!m_helloAccepted) {
            // First response is hello's.
            if (msg.value("ok", false)) {
                m_helloAccepted = true;
                m_connected = true;
                Log::Info("[Friends] connected as '%s'",
                          msg.value("name", "?").c_str());
                ResendPresence();
            } else {
                Log::Warning("[Friends] hello rejected: %s — token stale? "
                             "Log in again from the launcher.",
                             msg.value("error", "?").c_str());
                // Don't hammer the server with a dead token.
                m_running = false;
            }
            return;
        }

        const int corrId = msg.value("id", 0);
        {
            bool isJoinReply = false;
            int64_t friendId = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (corrId != 0 && corrId == m_pendingJoinRequestId) {
                    isJoinReply = true;
                    m_pendingJoinRequestId = 0;
                    friendId = m_pendingJoinFriendId;
                    m_pendingJoinFriendId = 0;
                }
            }

            if (isJoinReply) {
                JoinInfoResult result;
                result.ok = msg.value("ok", false);
                if (result.ok) {
                    result.world = msg.value("world", "");
                    result.relay = (msg.value("mode", "direct") == "relay");
                    if (result.relay) {
                        // Dial the service itself; the ticket pairs us with
                        // the host's outbound tunnel.
                        result.host = m_host;
                        result.port = m_port;
                        result.ticket = msg.value("ticket", "");
                    } else {
                        result.host = msg.value("host", "");
                        result.port = static_cast<uint16_t>(msg.value("port", 0));
                    }
                } else {
                    result.error = msg.value("error", "network");
                }

                // A direct address is a CANDIDATE, not a verdict — the service
                // hands out whatever it has and we find out whether it works.
                // friendId != 0 means this was the first ask, so the relay
                // retry is still available if the probe fails.
                if (result.ok && !result.relay && friendId != 0 && !result.host.empty()) {
                    ProbeDirectThenPublish(std::move(result), friendId);
                } else {
                    PublishJoinResult(std::move(result));
                }
                return;
            }
        }

        if (!msg.value("ok", true)) {
            // Surface op failures to the Friends screen in plain words.
            const std::string e = msg.value("error", "");
            std::string human =
                e == "not_found"       ? "No player with that name." :
                e == "self"            ? "That's you!" :
                e == "already_friends" ? "Already friends." :
                e == "already_pending" ? "Request already sent." :
                e == "not_hosting"     ? "That friend is not hosting a world." :
                e == "not_online"      ? "That friend is offline." :
                e == "no_request"      ? "That request is gone." :
                                         "Error: " + e;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = std::move(human);
        }
    }

    // ── UI-facing API ───────────────────────────────────────────────────────

    FriendsRoster FriendsClient::GetRoster() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_roster;
    }

    std::vector<FriendInvite> FriendsClient::ConsumeInvites() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<FriendInvite> out;
        out.swap(m_pendingInvites);
        return out;
    }

    bool FriendsClient::LatestInvite(FriendInvite& out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_hasLatestInvite) return false;
        out = m_latestInvite;
        return true;
    }

    void FriendsClient::ClearLatestInvite() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hasLatestInvite = false;
    }

    void FriendsClient::SendFriendRequest(const std::string& name) {
        net::post(m_ioContext, [this, name] {
            nlohmann::json req{{"op", "friend_request"}, {"name", name},
                               {"id", m_nextRequestId++}};
            SendJson(req.dump());
        });
    }

    void FriendsClient::SendOp(const char* op, int64_t friendId) {
        std::string opStr = op;
        net::post(m_ioContext, [this, opStr, friendId] {
            nlohmann::json req{{"op", opStr}, {"friend", friendId},
                               {"id", m_nextRequestId++}};
            SendJson(req.dump());
        });
    }

    void FriendsClient::AcceptRequest(int64_t friendId)  { SendOp("friend_accept", friendId); }
    void FriendsClient::DeclineRequest(int64_t friendId) { SendOp("friend_decline", friendId); }
    void FriendsClient::RemoveFriend(int64_t friendId)   { SendOp("friend_remove", friendId); }
    void FriendsClient::SendInvite(int64_t friendId)     { SendOp("invite", friendId); }

    void FriendsClient::SetPresence(FriendPresence::State state,
                                    const std::string& world, uint16_t hostPort,
                                    const std::string& externalIp) {
        m_uiPresence = static_cast<int>(state);
        net::post(m_ioContext, [this, state, world, hostPort, externalIp] {
            m_presenceState = state;
            m_presenceWorld = world;
            m_presencePort = hostPort;
            m_presenceExternalIp = externalIp;
            if (!m_connected) return;   // re-sent on reconnect
            ResendPresence();
        });
    }

    void FriendsClient::ResendPresence() {
        nlohmann::json req{{"op", "presence"},
                           {"state", StateName(m_presenceState)},
                           {"world", m_presenceWorld},
                           {"port", m_presencePort},
                           {"external_ip", m_presenceExternalIp},
                           {"id", m_nextRequestId++}};
        SendJson(req.dump());
    }

    void FriendsClient::DialRelay(const std::string& ticket) {
        if (!m_relayHandler) {
            Log::Warning("[Friends] relay_open ignored — no server to adopt it");
            return;
        }
        // A SECOND connection to the service, separate from the control
        // session: after the attach handshake it carries raw game bytes.
        auto socket = std::make_shared<net::ip::tcp::socket>(m_ioContext);
        auto resolver = std::make_shared<net::ip::tcp::resolver>(m_ioContext);

        resolver->async_resolve(m_host, std::to_string(m_port),
            [this, socket, resolver, ticket](const error_code& ec,
                                             net::ip::tcp::resolver::results_type results) {
                if (ec) {
                    Log::Warning("[Friends] relay dial: resolve failed");
                    return;
                }
                net::async_connect(*socket, results,
                    [this, socket, ticket](const error_code& ec2,
                                           const net::ip::tcp::endpoint&) {
                        if (ec2) {
                            Log::Warning("[Friends] relay dial: connect failed");
                            return;
                        }
                        nlohmann::json attach{{"op", "relay_attach"},
                                              {"role", "host"},
                                              {"ticket", ticket}};
                        auto payload = std::make_shared<std::string>(attach.dump() + "\n");
                        net::async_write(*socket, net::buffer(*payload),
                            [this, socket, payload](const error_code& ec3, std::size_t) {
                                if (ec3) {
                                    Log::Warning("[Friends] relay dial: write failed");
                                    return;
                                }
                                // Consume the one-line ack so the game
                                // protocol starts on a clean stream.
                                auto ack = std::make_shared<net::streambuf>();
                                net::async_read_until(*socket, *ack, '\n',
                                    [this, socket, ack](const error_code& ec4, std::size_t) {
                                        if (ec4) {
                                            Log::Warning("[Friends] relay dial: no ack");
                                            return;
                                        }
                                        std::istream is(ack.get());
                                        std::string line;
                                        std::getline(is, line);
                                        if (line.find("\"ok\":true") == std::string::npos) {
                                            Log::Warning("[Friends] relay attach rejected: %s",
                                                         line.c_str());
                                            return;
                                        }
                                        // Hand the live socket to the game
                                        // server. release() detaches it from
                                        // this io_context without closing.
                                        error_code releaseError;
                                        auto handle = socket->release(releaseError);
                                        if (releaseError) {
                                            Log::Error("[Friends] relay socket release failed: %s",
                                                       releaseError.message().c_str());
                                            return;
                                        }
                                        Log::Info("[Friends] relay tunnel established — "
                                                  "handing socket to the server");
                                        m_relayHandler(handle);
                                    });
                            });
                    });
            });
    }

    void FriendsClient::RequestJoinInfo(int64_t friendId) {
        net::post(m_ioContext, [this, friendId] {
            const int corrId = m_nextRequestId++;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingJoinRequestId = corrId;
                // Remembered so a failed direct probe can re-ask for a relay.
                m_pendingJoinFriendId = friendId;
            }
            nlohmann::json req{{"op", "join_info"}, {"friend", friendId},
                               {"id", corrId}};
            SendJson(req.dump());
        });
    }

    void FriendsClient::PublishJoinResult(JoinInfoResult result) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_joinResults.push_back(std::move(result));
    }

    void FriendsClient::ProbeDirectThenPublish(JoinInfoResult direct, int64_t friendId) {
        // One TCP connect against the host's game port. This is the test the
        // SERVICE used to run and could not run correctly — see the header.
        //
        // Budget is deliberately short: a dead address must not stall the join
        // behind a full OS connect timeout (~75 s on some stacks) when the
        // relay is sitting right there. A reachable host on any normal
        // connection answers well inside this.
        constexpr int kProbeTimeoutSeconds = 3;

        auto socket   = std::make_shared<net::ip::tcp::socket>(m_ioContext);
        auto timer    = std::make_shared<net::steady_timer>(m_ioContext);
        auto resolver = std::make_shared<net::ip::tcp::resolver>(m_ioContext);
        // Guards against the timeout and the connect both firing.
        auto settled  = std::make_shared<bool>(false);

        auto finish = [this, socket, timer, settled](bool reachable,
                                                     JoinInfoResult direct,
                                                     int64_t friendId) {
            if (*settled) return;
            *settled = true;
            timer->cancel();
            error_code ignored;
            socket->close(ignored);

            if (reachable) {
                Log::Info("[Friends] direct probe to %s:%u succeeded — joining directly",
                          direct.host.c_str(), static_cast<unsigned>(direct.port));
                PublishJoinResult(std::move(direct));
                return;
            }

            Log::Info("[Friends] direct probe to %s:%u failed — asking for a relay",
                      direct.host.c_str(), static_cast<unsigned>(direct.port));
            // Re-ask with force_relay. friendId is NOT stored this time, so the
            // reply is published as-is and cannot loop.
            const int corrId = m_nextRequestId++;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingJoinRequestId = corrId;
                m_pendingJoinFriendId = 0;
            }
            nlohmann::json req{{"op", "join_info"}, {"friend", friendId},
                               {"id", corrId}, {"force_relay", true}};
            SendJson(req.dump());
        };

        timer->expires_after(std::chrono::seconds(kProbeTimeoutSeconds));
        timer->async_wait([finish, direct, friendId](const error_code& ec) {
            if (ec) return;   // cancelled because the connect already settled
            finish(false, direct, friendId);
        });

        resolver->async_resolve(direct.host, std::to_string(direct.port),
            [this, socket, resolver, finish, direct, friendId](
                    const error_code& ec, net::ip::tcp::resolver::results_type results) {
                if (ec) { finish(false, direct, friendId); return; }
                net::async_connect(*socket, results,
                    [finish, direct, friendId](const error_code& ec2,
                                               const net::ip::tcp::endpoint&) {
                        finish(!ec2, direct, friendId);
                    });
            });
    }

    bool FriendsClient::PollJoinResult(JoinInfoResult& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_joinResults.empty()) return false;
        out = std::move(m_joinResults.front());
        m_joinResults.erase(m_joinResults.begin());
        return true;
    }

    std::string FriendsClient::ConsumeLastError() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string out = std::move(m_lastError);
        m_lastError.clear();
        return out;
    }

} // namespace Client
