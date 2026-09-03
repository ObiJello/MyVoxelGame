// File: src/server/network/ServerConnection.cpp
#include "ServerConnection.hpp"
#include "NetworkServer.hpp"
#include "../commands/CommandDispatcher.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "listeners/HandshakePacketListener.hpp"
#include "listeners/LoginPacketListener.hpp"
#include "listeners/ServerPlayPacketListener.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Assert.hpp"
#include "common/core/Log.hpp"
#include <limits>
#include "common/network/packets/HandshakeC2S.hpp"
#include "common/network/packets/LoginStartC2S.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include "common/network/packets/C2SPackets.hpp"
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include "common/network/PacketRegistry.hpp"
#include "../IntegratedServer.hpp"

namespace Server {

    // Static connection ID counter
    std::atomic<uint32_t> ServerConnection::s_nextConnectionId{1};

    ServerConnection::ServerConnection(tcp::socket socket, NetworkServer* server)
        : NetworkConnection(std::move(socket))
        , m_server(server)
        , m_connectionId(s_nextConnectionId.fetch_add(1))
    {
        // Set connection name to "Server#id" for clearer logging
        SetName("Server#" + std::to_string(m_connectionId));
        
        m_lastActivity = std::chrono::steady_clock::now();
        // MC's constructor seeds keepAliveTime so the FIRST challenge goes out
        // one full interval after the listener exists, not immediately.
        const int64_t nowMs = NowMs();
        m_keepAliveTime.store(nowMs, std::memory_order_relaxed);
        m_lastPacketReceivedMs.store(nowMs, std::memory_order_relaxed);
        
        // Start with handshake listener
        setProtocolState(Network::ProtocolState::HANDSHAKING);
        
        // Register packet handlers (legacy - will be replaced by listener system)
        using namespace Network;
        // DISABLED: These conflict with the new listener system
        // m_packetRegistry.RegisterHandler(PacketId::Handshake,
        //                                  [this](const std::vector<uint8_t>& p) { HandleHandshake(p); });
        // m_packetRegistry.RegisterHandler(PacketId::LoginStart,
        //                                  [this](const std::vector<uint8_t>& p) { HandleLoginStart(p); });
        // Only ONE entry left. Everything else that used to live here — block
        // actions, movement, chat, held-item, inventory clicks, the teleport
        // ack — now has a typed representation and is decoded in DecodePacket,
        // which routes it through the queue to the server thread. Handlers
        // registered here run INLINE ON THE NETWORK I/O THREAD (unless
        // ShouldDeferPacket says otherwise), so nothing that touches the level,
        // a player or a session may be added back.
        //
        // Client settings survive because they can legitimately arrive before
        // the session exists, and MC handles that same pre-play case on its
        // network thread too (ServerConfigurationPacketListenerImpl
        // .handleClientInformation carries no ensureRunningOnSameThread call,
        // while the game-phase handler at ServerGamePacketListenerImpl:1953
        // does). Once in PLAY, DecodePacket claims it and this never fires.
        m_packetRegistry.RegisterHandler(PacketId::ClientConfigC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleClientSettings(p); });

        // Answered on the I/O thread, never deferred — see ShouldDeferPacket
        // and HandleKeepAliveResponse. This is what keeps the keep-alive
        // answerable while the tick thread is stalled.
        m_packetRegistry.RegisterHandler(PacketId::KeepAliveC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleKeepAliveResponse(p); });
    }

    ServerConnection::~ServerConnection() {
        // Don't call shared_from_this() in destructor as it can throw
        // Connection cleanup is handled by OnDisconnected() callback
    }

    void ServerConnection::OnConnected() {
        Log::Info("[ServerConnection %u] Connected from %s", 
            GetConnectionId(), GetRemoteEndpoint().address().to_string().c_str());
        
        if (m_server) {
            m_server->OnConnectionEstablished(
                std::static_pointer_cast<ServerConnection>(shared_from_this()));
        }
    }

    void ServerConnection::OnDisconnected() {
        Log::Info("[ServerConnection %u] Disconnected (%s)", 
            GetConnectionId(), m_playerName.empty() ? "unnamed" : m_playerName.c_str());
        
        if (m_server) {
            try {
                // Safely get shared_ptr - this can throw if we're being destroyed
                auto self = shared_from_this();
                m_server->OnConnectionClosed(
                    std::static_pointer_cast<ServerConnection>(self));
            } catch (const std::bad_weak_ptr& e) {
                // Object is being destroyed, can't get shared_ptr
                Log::Debug("[ServerConnection %u] Already being destroyed, skipping server notification", 
                    GetConnectionId());
            } catch (const std::exception& e) {
                // Other errors during notification
                Log::Warning("[ServerConnection %u] Failed to notify server of disconnection: %s", 
                    GetConnectionId(), e.what());
            }
        }
    }

    void ServerConnection::OnError(const error_code& error) {
        Log::Error("[ServerConnection %u] Error: %s", 
            GetConnectionId(), error.message().c_str());
    }
    
    void ServerConnection::tick() {
        // MC ServerGamePacketListenerImpl.tickCount, incremented once per
        // server tick. Drives the teleport retry window in
        // UpdateAwaitingTeleport; kept here rather than on the session because
        // the teleport gate lives on the connection.
        ++m_tickCount;

        // Drain the WHOLE incoming queue, as MC does. No per-tick packet cap
        // and no time budget — see the note further down where the budget used
        // to be. Peek-then-pop is kept: it is what makes the drain lossless.
        int packetsProcessed = 0;
        for (;;) {
            // Free any listener displaced during the PREVIOUS iteration. Safe
            // here and nowhere earlier: the handler that triggered the switch
            // has returned by now, so nothing is still executing inside it.
            m_retiredListeners.clear();

            // Check if queue is empty
            if (!HasIncomingPackets()) {
                break;
            }
            
            // NO TIME BUDGET. MC drains the whole queue every tick —
            // BlockableEventLoop.runAllTasks() is literally
            // "while (this.pollTask()) {}" (util/thread/BlockableEventLoop
            // .java:109-113) — and the budget that used to sit here was our own
            // invention. It was also actively harmful: the criticality test
            // only PEEKED AT THE HEAD of the queue, so a KeepAliveC2S sitting
            // behind non-critical packets never got its exemption, and with
            // multi-second ticks only a handful of 1 ms drains fitted inside
            // the timeout window. That is what disconnected a player from their
            // own singleplayer world.
            //
            // The trade, stated honestly: the tick after a stall now handles
            // the whole accumulated backlog at once and will show up as one
            // long tick rather than a slow bleed. That is MC's behaviour, and
            // the queue is unbounded and lossless either way, so the budget was
            // only ever deferring the same work forever.
            
            // NOW we're committed to processing this packet, so pop it
            Network::IncomingPacket packet;
            if (!TryPopIncoming(packet)) {
                // Shouldn't happen since we checked HasIncomingPackets, but be safe
                Log::Warning("[ServerConnection %u] Failed to pop packet from non-empty queue", GetConnectionId());
                break;
            }
            
            // Process the packet
            if (packet.packet) {
                // An undecoded packet that was deferred here rather than run on
                // the I/O thread — dispatch it to the legacy registry NOW, on
                // the server thread. Goes away with the registry itself.
                if (auto* raw = dynamic_cast<Network::RawPayloadPacket*>(packet.packet.get())) {
                    try {
                        if (!m_packetRegistry.HandlePacket(raw->rawId(), raw->payload())) {
                            Log::Warning("[ServerConnection %u] Unhandled deferred packet ID 0x%02X in phase %d",
                                        GetConnectionId(), raw->rawId(), static_cast<int>(m_phase.load()));
                        }
                    } catch (const std::exception& e) {
                        Log::Error("[ServerConnection %u] Exception processing deferred packet 0x%02X: %s",
                                  GetConnectionId(), raw->rawId(), e.what());
                    }
                    packetsProcessed++;
                    continue;
                }

                // Check if we need to create a listener based on the packet type
                if (!m_listener) {
                    if (packet.packet->getId() == Network::PacketId::Handshake) {
                        RetireListener();
                        m_listener = std::make_unique<HandshakePacketListener>(*this);
                    } else if (packet.packet->getId() == Network::PacketId::LoginStart && 
                               m_phase == ConnectionPhase::LOGIN) {
                        RetireListener();
                        m_listener = std::make_unique<LoginPacketListener>(*this, m_server);
                    }
                }
                
                if (m_listener) {
                    try {
                        // Apply packet to current listener (visitor pattern)
                        if (auto* c2sPacket = dynamic_cast<Network::IC2SPacket*>(packet.packet.get())) {
                            // Store listener name before apply (it might change during apply)
                            std::string listenerName = m_listener->getName();
                            
                            // Apply the packet - this might change m_listener!
                            c2sPacket->apply(*m_listener);
                            packetsProcessed++;
                            
                            // Log if listener changed (for debugging)
                            if (m_listener && m_listener->getName() != listenerName) {
                                Log::Debug("[ServerConnection %u] Listener changed from %s to %s after packet 0x%02X",
                                          GetConnectionId(), listenerName.c_str(), m_listener->getName(),
                                          static_cast<int>(packet.packet->getId()));
                            }
                        }
                    } catch (const std::exception& e) {
                        Log::Error("[ServerConnection %u] Exception processing packet: %s", 
                                  GetConnectionId(), e.what());
                    }
                } else {
                    Log::Warning("[ServerConnection %u] No listener set for packet ID 0x%02X in phase %d", 
                                GetConnectionId(), static_cast<int>(packet.packet->getId()), 
                                static_cast<int>(m_phase.load()));
                }
            }
        }
        
        // ...and once more for the final iteration's switch, if any.
        m_retiredListeners.clear();

        // Log only if we processed packets or have a backlog
        if (packetsProcessed > 0 || GetIncomingQueueSize() > 10) {
            Log::Debug("[ServerConnection %u] Processed %d packets, %zu remaining in queue", 
                      GetConnectionId(), packetsProcessed, GetIncomingQueueSize());
        }
        
        // A bad keep-alive seen on the I/O thread asks for the kick here, on
        // the tick thread, because the teardown touches server state.
        if (m_pendingTimeoutDisconnect.exchange(false, std::memory_order_relaxed)) {
            Log::Warning("[ServerConnection %u] Unsolicited or mismatched keep-alive - disconnecting",
                         GetConnectionId());
            SendDisconnect("Timed out");
            return;
        }

        // MC ServerCommonPacketListenerImpl.keepConnectionAlive:117-132,
        // transliterated:
        //
        //   if (!isSingleplayerOwner() && now - keepAliveTime >= 15000) {
        //       if (keepAlivePending) disconnect(TIMEOUT);
        //       else { keepAlivePending = true; keepAliveTime = now;
        //              keepAliveChallenge = now; send(KeepAlive(challenge)); } }
        //
        // Two things this buys that the old two-timer version did not:
        //
        // 1. The singleplayer owner is exempt OUTRIGHT. That is why vanilla
        //    never times the host out however long a tick runs, and it is the
        //    direct fix for a host being kicked out of their own world after
        //    summoning a million entities.
        // 2. ONE timer drives both halves, so a stalled tick thread freezes the
        //    send and the timeout together. The old code armed the deadline on
        //    a real-time clock and then could not answer it, because only the
        //    tick thread ever looked at the reply.
        if (m_phase == ConnectionPhase::PLAY && !IsSingleplayerOwner()) {
            const int64_t now = NowMs();
            if (now - m_keepAliveTime.load(std::memory_order_relaxed) >= KEEP_ALIVE_INTERVAL_MS) {
                if (m_keepAlivePending.load(std::memory_order_relaxed)) {
                    Log::Warning("[ServerConnection %u] Keep-alive timeout - no response to challenge %llu",
                                 GetConnectionId(),
                                 (unsigned long long)m_keepAliveChallenge.load(std::memory_order_relaxed));
                    SendDisconnect("Timed out");
                    return;   // the session is torn down underneath us
                }
                // MC uses the send timestamp ITSELF as the challenge id.
                m_keepAliveTime.store(now, std::memory_order_relaxed);
                m_keepAliveChallenge.store(static_cast<uint64_t>(now), std::memory_order_relaxed);
                m_keepAlivePending.store(true, std::memory_order_relaxed);
                Network::PacketBuffer buf;
                buf.WriteLong(static_cast<uint64_t>(now));
                SendPacket(static_cast<uint8_t>(Network::PacketId::KeepAliveS2C), buf.GetData());
                Log::Debug("[ServerConnection %u] Sent keep-alive challenge %lld",
                           GetConnectionId(), (long long)now);
            }
        }
        
        // Check for timeout
        if (IsTimedOut()) {
            Log::Warning("[ServerConnection %u] Connection timed out", GetConnectionId());
            SendDisconnect("Timed out");
        }
    }
    
    void ServerConnection::setProtocolState(Network::ProtocolState state) {
        Log::Info("[ServerConnection %u] Switching protocol state to %d", 
                  GetConnectionId(), static_cast<int>(state));
        
        // Update phase (for legacy compatibility)
        switch (state) {
            case Network::ProtocolState::HANDSHAKING:
                m_phase = ConnectionPhase::HANDSHAKING;
                RetireListener();
                m_listener = std::make_unique<HandshakePacketListener>(*this);
                break;
                
            case Network::ProtocolState::STATUS:
                m_phase = ConnectionPhase::STATUS;
                // TODO: m_listener = std::make_unique<StatusPacketListener>(*this);
                Log::Warning("[ServerConnection %u] STATUS listener not implemented yet", GetConnectionId());
                break;
                
            case Network::ProtocolState::LOGIN:
                m_phase = ConnectionPhase::LOGIN;
                RetireListener();
                m_listener = std::make_unique<LoginPacketListener>(*this, m_server);
                break;
                
            case Network::ProtocolState::PLAY:
                // PLAY state requires a session to be passed via the overload
                Log::Error("[ServerConnection %u] Cannot switch to PLAY state without session", GetConnectionId());
                SendDisconnect("Server error: No session available for PLAY state");
                break;
        }
    }
    
    void ServerConnection::setProtocolState(Network::ProtocolState state, PlayerSession* session) {
        Log::Info("[ServerConnection %u] Switching protocol state to %d with session %u", 
                  GetConnectionId(), static_cast<int>(state), session ? session->GetPlayerId() : 0);
        
        // This overload is only for PLAY state with a session
        if (state != Network::ProtocolState::PLAY) {
            Log::Warning("[ServerConnection %u] Session-aware protocol switch only supports PLAY state", 
                        GetConnectionId());
            setProtocolState(state);  // Fall back to regular version
            return;
        }
        
        if (!session) {
            Log::Error("[ServerConnection %u] No session provided for PLAY state", 
                      GetConnectionId());
            SendDisconnect("Server error: No session available");
            return;
        }
        
        // Update phase
        m_phase = ConnectionPhase::PLAY;
        
        // Create listener with session reference
        RetireListener();
        m_listener = std::make_unique<ServerPlayPacketListener>(*this, *session);
        
        Log::Info("[ServerConnection %u] Switched to PLAY state with session-aware ServerPlayPacketListener", 
                 GetConnectionId());
    }
    
    void ServerConnection::sendInitialGameData() {
        Log::Debug("[ServerConnection %u] Sending initial game data", GetConnectionId());
        
        // Send time update
        SendCurrentTimeUpdate();

        // Send player abilities + the world's game mode
        SendPlayerAbilitiesForJoin();

        // Send spawn position
        Network::PacketBuffer spawnBuffer;
        spawnBuffer.WriteInt(0); // X
        spawnBuffer.WriteInt(67); // Y
        spawnBuffer.WriteInt(0); // Z
        SendPacket(static_cast<uint8_t>(Network::PacketId::WorldSpawn), spawnBuffer.GetData());
    }

    bool ServerConnection::ShouldDeferPacket(uint8_t packetId) const {
        // MC's split, exactly: ServerGamePacketListenerImpl defers every
        // handler; ServerLoginPacketListenerImpl defers none. Anything arriving
        // while we are still HANDSHAKING/LOGIN is connection-setup work that
        // belongs on the I/O thread, and anything arriving in PLAY touches the
        // level or the player and belongs on the server thread.
        if (m_phase != ConnectionPhase::PLAY) {
            return false;
        }
        // Disconnect is terminal — MC's handleDisconnect carries no
        // ensureRunningOnSameThread call and tears the connection down from the
        // Netty thread. Deferring it would keep reading from a dead peer.
        //
        // KeepAliveC2S joins it for the same reason: handleKeepAlive is the
        // other method in ServerCommonPacketListenerImpl with NO
        // ensureRunningOnSameThread call, so MC answers it inline on the Netty
        // thread. Deferring it to the tick is what let an 11.7 s tick eat the
        // response and time the player out of their own singleplayer world.
        return packetId != static_cast<uint8_t>(Network::PacketId::Disconnect)
            && packetId != static_cast<uint8_t>(Network::PacketId::KeepAliveC2S);
    }

    void ServerConnection::OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) {
        UpdateActivity();
        // Every inbound FRAME refreshes the read timeout, which is what makes
        // it netty's ReadTimeoutHandler rather than a keep-alive echo watcher.
        m_lastPacketReceivedMs.store(NowMs(), std::memory_order_relaxed);
        
        // Forward to server for statistics
        if (m_server) {
            m_server->OnPacketReceived(
                std::static_pointer_cast<ServerConnection>(shared_from_this()),
                packetId, payload);
        }
        
        // Handle packet based on current phase
        if (!m_packetRegistry.HandlePacket(packetId, payload)) {
            Log::Warning("[ServerConnection %u] Unhandled packet ID: 0x%02X in phase %d",
                GetConnectionId(), packetId, static_cast<int>(m_phase.load()));
        }
    }

    // ========================================================================
    // PACKET SENDING (SERVER → CLIENT)
    // ========================================================================

    void ServerConnection::SendBlockChange(const Network::BlockChangeS2CPacket& packet) {
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::BlockChangeS2C), data);
    }

    void ServerConnection::SendChatMessage(const std::string& message, uint8_t position, uint32_t senderId) {
        SendChatMessage(Network::ChatMessageS2CPacket(message, position, senderId));
    }

    void ServerConnection::SendChatMessage(const Network::ChatMessageS2CPacket& packet) {
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ChatMessageS2C), data);
    }


    void ServerConnection::SendDisconnect(const std::string& reason) {
        Network::PacketBuffer buffer;
        buffer.WriteString(reason);
        SendPacket(static_cast<uint8_t>(Network::PacketId::Disconnect), buffer.GetData());

        // No sleep. This used to block the CALLING thread for 100 ms hoping the
        // packet had drained — which meant the server tick thread stalled for a
        // tenth of a second on every kick, and the wait was a guess either way.
        // SendPacket posts to the connection's strand, so ordering is already
        // guaranteed: the Disconnect frame is queued ahead of the teardown and
        // goes out first.
        //
        // TODO (parity gap, out of scope here): MC additionally latches the
        // connection read-only at this point so in-flight inbound packets are
        // dropped rather than handled after the disconnect decision
        // (PacketListener.shouldHandleMessage, PacketListener.java:29-30).
        Disconnect();
    }

    void ServerConnection::SendTimeUpdate(uint64_t worldAge, uint64_t timeOfDay, bool doDaylightCycle) {
        Network::PacketBuffer buffer;
        buffer.WriteLong(worldAge);
        buffer.WriteLong(timeOfDay);
        buffer.WriteByte(doDaylightCycle ? 1 : 0);
        SendPacket(static_cast<uint8_t>(Network::PacketId::TimeUpdate), buffer.GetData());
    }

    void ServerConnection::SendCurrentTimeUpdate() {
        uint64_t gameTime = 0;
        uint64_t dayTime = 6000;
        bool doDaylightCycle = false;
        if (Server::g_integratedServer && Server::g_integratedServer->GetWorld()) {
            const auto* world = Server::g_integratedServer->GetWorld();
            gameTime = static_cast<uint64_t>(world->GetGameTime());
            dayTime = static_cast<uint64_t>(world->GetDayTime());
            doDaylightCycle = world->GetDoDaylightCycle();
        }
        SendTimeUpdate(gameTime, dayTime, doDaylightCycle);
    }

    namespace {
        // MC ClientboundPlayerAbilitiesPacket built from the live Abilities
        // (+ our extra gameMode byte, replacing CHANGE_GAME_MODE). The
        // mode→flag rules mirror GameType.updatePlayerAbilities
        // (GameType.java:62-80); keeping them in one place stops the
        // login-time packet and the post-join packet from disagreeing about
        // what "creative" means.
        Network::PlayerAbilitiesS2CPacket BuildAbilitiesPacket(GameMode mode,
                                                               bool flying, bool canFly,
                                                               bool noclip = false,
                                                               float scale = 1.0f) {
            Network::PlayerAbilitiesS2CPacket packet;
            packet.scale = scale;
            if (noclip) packet.flags |= Network::PlayerAbilitiesS2CPacket::FLAG_NOCLIP;
            if (mode == GameMode::CREATIVE || mode == GameMode::SPECTATOR) {
                packet.flags |= Network::PlayerAbilitiesS2CPacket::FLAG_INVULNERABLE;
            }
            if (flying) packet.flags |= Network::PlayerAbilitiesS2CPacket::FLAG_FLYING;
            if (canFly) packet.flags |= Network::PlayerAbilitiesS2CPacket::FLAG_MAY_FLY;
            if (mode == GameMode::CREATIVE) {
                packet.flags |= Network::PlayerAbilitiesS2CPacket::FLAG_INSTABUILD;
            }
            packet.gameMode = static_cast<uint8_t>(mode);
            return packet;
        }
    } // namespace

    void ServerConnection::SendPlayerAbilities(const ServerPlayer& player) {
        auto data = Network::Serialization::Serialize(
            BuildAbilitiesPacket(player.getGameMode(), player.isFlying(), player.canFly(),
                                 player.isNoclip(), player.getScale()));
        SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerAbilities), data);
    }

    void ServerConnection::SendPlayerAbilitiesForJoin() {
        // The ServerPlayer doesn't exist yet here, but the WORLD's game mode
        // does — so send the real one rather than a survival placeholder.
        //
        // MC carries gameType in ClientboundLoginPacket's CommonPlayerSpawnInfo
        // and applies it inside the same handler that builds the level
        // (ClientPacketListener.handleLogin:508), so a vanilla client is never
        // in a state where the world exists but the mode is unknown. This used
        // to send survival unconditionally, which meant a creative player
        // joining got hearts and hunger drawn for the whole gap between login
        // and OnPlayerJoined's real abilities packet.
        //
        // Flags follow ServerPlayer::setGameMode, which mirrors the same MC
        // GameType.updatePlayerAbilities rules: only spectator starts flying.
        GameMode mode = GameMode::SURVIVAL;
        if (g_integratedServer) {
            mode = static_cast<GameMode>(g_integratedServer->GetConfig().defaultGameMode);
        }
        const bool canFly = (mode == GameMode::CREATIVE || mode == GameMode::SPECTATOR);
        const bool flying = (mode == GameMode::SPECTATOR);

        auto data = Network::Serialization::Serialize(
            BuildAbilitiesPacket(mode, flying, canFly));
        SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerAbilities), data);
    }

    // ========================================================================
    // PACKET HANDLERS (CLIENT → SERVER)
    // ========================================================================

    void ServerConnection::HandleHandshake(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::HANDSHAKING) {
            Log::Warning("[ServerConnection %u] Unexpected handshake in phase %d",
                GetConnectionId(), static_cast<int>(m_phase.load()));
            return;
        }
        
        Network::PacketReader reader(payload);
        uint32_t protocolVersion = reader.ReadVarInt();
        std::string serverAddress = reader.ReadString();
        uint16_t serverPort = reader.ReadShort();
        uint32_t nextState = reader.ReadVarInt();
        
        Log::Info("[ServerConnection %u] Handshake: protocol=%u, address=%s:%u, nextState=%u",
            GetConnectionId(), protocolVersion, serverAddress.c_str(), serverPort, nextState);
        
        // Set next phase
        if (nextState == 1) {
            m_phase = ConnectionPhase::STATUS;
        } else if (nextState == 2) {
            m_phase = ConnectionPhase::LOGIN;
        } else {
            SendDisconnect("Invalid handshake state");
        }
    }

    void ServerConnection::HandleLoginStart(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::LOGIN) {
            Log::Warning("[ServerConnection %u] Unexpected login start in phase %d",
                GetConnectionId(), static_cast<int>(m_phase.load()));
            return;
        }
        
        Network::PacketReader reader(payload);
        m_playerName = reader.ReadString();
        // Optional trailing colour byte (Game::PlayerColorId). Old clients don't
        // send it — those default to 0 (Default neon green).
        if (reader.Remaining() >= 1) {
            m_playerColor = reader.ReadByte();
        }

        Log::Info("[ServerConnection %u] Player login: %s (color id=%u)",
            GetConnectionId(), m_playerName.c_str(), static_cast<unsigned>(m_playerColor));
        
        // Simple authentication (accept everyone for now)
        m_authenticated = true;
        m_playerId = GetConnectionId(); // Use connection ID as player ID
        m_phase = ConnectionPhase::PLAY;
        
        Log::Info("[ServerConnection %u] Player '%s' AUTHENTICATED (m_authenticated=%s), phase=PLAY", 
                  GetConnectionId(), m_playerName.c_str(), m_authenticated ? "true" : "false");
        
        // Send login success
        Network::PacketBuffer buffer;
        buffer.WriteString(std::to_string(m_playerId)); // UUID as string
        buffer.WriteString(m_playerName);
        Log::Debug("[ServerConnection %u] Sending LoginSuccess packet", GetConnectionId());
        SendPacket(static_cast<uint8_t>(Network::PacketId::LoginSuccess), buffer.GetData());
        
        // Send initial game data
        Log::Debug("[ServerConnection %u] Sending initial game data", GetConnectionId());
        SendCurrentTimeUpdate();
        SendPlayerAbilitiesForJoin(); // Real game mode — OnPlayerJoined reconfirms
        
        // Send spawn position
        Network::PacketBuffer spawnBuffer;
        spawnBuffer.WriteInt(0); // X
        spawnBuffer.WriteInt(67); // Y
        spawnBuffer.WriteInt(0); // Z
        Log::Debug("[ServerConnection %u] Sending WorldSpawn packet", GetConnectionId());
        SendPacket(static_cast<uint8_t>(Network::PacketId::WorldSpawn), spawnBuffer.GetData());
        
        // Notify server that player has joined and needs initial chunks
        if (m_server) {
            Log::Info("[ServerConnection %u] Notifying server of new player join", GetConnectionId());
            m_server->OnPlayerJoined(std::static_pointer_cast<ServerConnection>(shared_from_this()));
        }
    }



    void ServerConnection::HandleChatMessage(const Network::ChatMessageC2SPacket& packet) {
        // Server thread — reached from ServerPlayPacketListener::onChatMessageC2S
        // via the typed packet queue. The phase/auth checks that used to open
        // this method are now in DecodePacket, which only builds the packet in
        // PLAY with an authenticated connection.
        ASSERT_SERVER_THREAD();

        Log::Info("[Server#%u] RECEIVED ChatMessageC2S (ID: 0x%02X) - Message: %s (isCommand=%d)",
                  GetConnectionId(), static_cast<uint8_t>(Network::PacketId::ChatMessageC2S),
                  packet.message.c_str(), packet.isCommand);

        // Route commands to the dispatcher (MC: separate ServerboundChatCommandPacket)
        if (packet.isCommand && Server::g_integratedServer) {
            // Strip leading '/' if present
            std::string cmdLine = packet.message;
            if (!cmdLine.empty() && cmdLine[0] == '/') {
                cmdLine = cmdLine.substr(1);
            }

            auto* sessionManager = Server::g_integratedServer->GetSessionManager();
            if (!sessionManager) return;
            auto session = sessionManager->GetSession(m_playerId);
            if (session && session->GetPlayer()) {
                Server::g_integratedServer->GetCommandDispatcher().ExecuteCommand(
                    cmdLine, *session->GetPlayer(), *this, *sessionManager);
            }
            return; // Commands are NOT broadcast as chat
        }

        // Forward to IntegratedServer for processing
        if (Server::g_integratedServer) {
            Server::g_integratedServer->ProcessChatMessage(packet);
        }

        // Broadcast chat to all connected players
        if (m_server) {
            std::string formattedMessage = "<" + m_playerName + "> " + packet.message;
            auto connections = m_server->GetConnections();

            std::vector<ServerConnectionPtr> activeConnections;
            for (auto& conn : connections) {
                if (conn && conn->GetState() != Network::ConnectionState::DISCONNECTED && conn->IsAuthenticated()) {
                    activeConnections.push_back(conn);
                }
            }

            for (auto& conn : activeConnections) {
                try {
                    conn->SendChatMessage(formattedMessage, 0, m_playerId);
                } catch (const std::exception& e) {
                    Log::Warning("Failed to send chat message to connection: %s", e.what());
                }
            }
        }
    }

    // MC ServerCommonPacketListenerImpl.handleKeepAlive:82-91.
    //
    // RUNS ON THE NETWORK I/O THREAD. It must touch nothing but its own
    // atomics — no PlayerSession, no world, no logging that reads game state.
    // MC has exactly the same constraint for exactly the same reason: this is
    // one of the two handlers in that class with no ensureRunningOnSameThread.
    void ServerConnection::HandleKeepAliveResponse(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        const uint64_t id = reader.ReadLong();

        if (m_keepAlivePending.load(std::memory_order_relaxed) &&
            id == m_keepAliveChallenge.load(std::memory_order_relaxed)) {
            // MC: latency = (latency * 3 + time) / 4
            const int32_t time =
                static_cast<int32_t>(NowMs() - m_keepAliveTime.load(std::memory_order_relaxed));
            m_latencyMs.store((m_latencyMs.load(std::memory_order_relaxed) * 3 + time) / 4,
                              std::memory_order_relaxed);
            m_keepAlivePending.store(false, std::memory_order_relaxed);
        } else {
            // MC disconnects on an unsolicited or wrong-id echo — unless this
            // is the singleplayer owner, who is outside the whole mechanism.
            // The teardown itself is deferred to the tick thread.
            if (!IsSingleplayerOwner()) {
                m_pendingTimeoutDisconnect.store(true, std::memory_order_relaxed);
            }
        }
    }




    void ServerConnection::Teleport(double x, double y, double z, float yRot, float xRot,
                                    double dx, double dy, double dz) {
        // Match MC's ServerGamePacketListenerImpl.teleport(PositionMoveRotation, Set<Relative>):
        //   1. Bump the awaiting-teleport id (wrap on int max)
        //   2. Snap the server-side player position
        //   3. Send ClientboundPlayerPosition to the client; client snaps and acks
        //
        // MC :1181 stamps the issue time FIRST, so the 20-tick retry window in
        // UpdateAwaitingTeleport is measured from this teleport and not an
        // earlier one.
        m_awaitingTeleportTime = m_tickCount;
        if (++m_awaitingTeleport == std::numeric_limits<int32_t>::max()) {
            m_awaitingTeleport = 0;
        }

        // Locate this connection's ServerPlayer via session manager and snap its position.
        // Use teleport() not setPosition() — setPosition() runs the anti-cheat
        // distance check (>100 blocks → reject + warn), which trips on every
        // legitimate server-issued teleport (portal jumps, /tp, world spawn
        // far from origin). teleport() bypasses the check.
        if (Server::g_integratedServer) {
            auto* sessionManager = Server::g_integratedServer->GetSessionManager();
            if (sessionManager) {
                auto session = sessionManager->GetSession(m_playerId);
                if (session && session->GetPlayer()) {
                    session->GetPlayer()->teleport(glm::dvec3(x, y, z));
                }
            }
        }

        // Gate: ignore the position in any further C2S move packet until the
        // client echoes the matching ack. Mirrors MC's
        // awaitingPositionFromClient — without this, 1–2 in-flight pre-teleport
        // MovePlayer packets revert m_position to the old location and other
        // clients see the teleported player flicker / stay behind. Cleared in
        // AcceptTeleportation when the id matches; re-sent by
        // UpdateAwaitingTeleport if the ack never comes.
        m_awaitingPositionFromClient = glm::dvec3(x, y, z);

        Network::ClientboundPlayerPositionPacket packet;
        packet.id = m_awaitingTeleport;
        packet.x = x;  packet.y = y;  packet.z = z;
        packet.dx = dx; packet.dy = dy; packet.dz = dz;
        packet.yRot = yRot;
        packet.xRot = xRot;
        packet.relatives = 0; // empty Set<Relative> — fully absolute teleport
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ClientboundPlayerPosition), data);

        Log::Info("[ServerConnection %u] Teleport id=%d → (%.2f, %.2f, %.2f) vel(%.2f, %.2f, %.2f)",
                  GetConnectionId(), m_awaitingTeleport, x, y, z, dx, dy, dz);
    }

    bool ServerConnection::UpdateAwaitingTeleport() {
        // MC ServerGamePacketListenerImpl.updateAwaitingTeleport (:1148).
        if (!m_awaitingPositionFromClient.has_value()) {
            // MC keeps the timestamp fresh while nothing is pending, so the
            // window is measured from the teleport rather than from whenever
            // the last one was cleared.
            m_awaitingTeleportTime = m_tickCount;
            return false;
        }

        if (m_tickCount - m_awaitingTeleportTime > 20) {
            const glm::dvec3 pos = *m_awaitingPositionFromClient;
            float yRot = 0.0f, xRot = 0.0f;
            if (Server::g_integratedServer) {
                if (auto* sessions = Server::g_integratedServer->GetSessionManager()) {
                    auto session = sessions->GetSession(m_playerId);
                    if (session && session->GetPlayer()) {
                        yRot = session->GetPlayer()->getYaw();
                        xRot = session->GetPlayer()->getPitch();
                    }
                }
            }
            Log::Warning("[ServerConnection %u] Teleport id=%d unacknowledged after 20 ticks "
                         "— re-sending", GetConnectionId(), m_awaitingTeleport);
            // Bumps the id, re-stamps m_awaitingTeleportTime and re-sends.
            Teleport(pos.x, pos.y, pos.z, yRot, xRot);
        }

        return true;
    }

    void ServerConnection::AcceptTeleportation(int32_t teleportId) {
        // No phase check. This runs on the server thread, applied from the
        // packet queue in FIFO order, so by the time it lands finalizeLogin has
        // already completed — but the packet itself may well have ARRIVED
        // during LOGIN, which is exactly the case the old phase check threw
        // away.
        if (teleportId == m_awaitingTeleport) {
            if (!m_awaitingPositionFromClient.has_value()) {
                // MC :514 — an ack for the current id with nothing outstanding
                // means the client is fabricating acks. MC disconnects.
                Log::Error("[ServerConnection %u] Teleport ack id=%d with no teleport pending",
                           GetConnectionId(), teleportId);
                SendDisconnect("Invalid player movement");
                return;
            }
            // Clear the gate — subsequent C2S move packets are honored in full again.
            m_awaitingPositionFromClient.reset();
            Log::Info("[ServerConnection %u] Teleport id=%d acked", GetConnectionId(), teleportId);
        } else {
            // Not an error: an ack for a SUPERSEDED teleport, which is exactly
            // what the 20-tick retry above produces when the original was slow
            // rather than lost. Ignoring it leaves the newer teleport pending,
            // which is correct.
            Log::Debug("[ServerConnection %u] Stale teleport ack: got %d, expected %d",
                       GetConnectionId(), teleportId, m_awaitingTeleport);
        }
    }

    void ServerConnection::HandleClientSettings(const std::vector<uint8_t>& payload) {
        // The PRE-PLAY path only; DecodePacket claims this id once the
        // connection reaches PLAY. Runs on the network I/O thread, as MC's
        // configuration-phase handleClientInformation does — at this point
        // there is no session to touch, and OnClientSettingsReceived parks the
        // value until one exists.
        Network::PacketReader reader(payload);
        const int renderDistance = reader.ReadVarInt();
        const bool vsync = reader.ReadByte() != 0;
        const float mouseSensitivity = reader.ReadFloat();
        const int simulationDistance = ReadSimulationDistance(reader, renderDistance);
        ApplyClientSettings(std::clamp(renderDistance, 2, 32), simulationDistance,
                            vsync, mouseSensitivity);
    }

    int ServerConnection::ReadSimulationDistance(Network::PacketReader& reader, int renderDistance) {
        // Appended after the original three fields. A client built before the
        // field existed sends none, and for it the old behaviour — simulate
        // everything it can see — is exactly what it expects.
        if (!reader.HasMore()) return std::clamp(renderDistance, 2, 32);
        return std::clamp(static_cast<int>(reader.ReadVarInt()), 2, 32);
    }

    void ServerConnection::ApplyClientSettings(int renderDistance, int simulationDistance,
                                               bool vsync, float mouseSensitivity) {
        Log::Info("[Server#%u] Client settings: renderDistance=%d, simulationDistance=%d, "
                  "vsync=%s, sensitivity=%.2f",
                  GetConnectionId(), renderDistance, simulationDistance,
                  vsync ? "true" : "false", mouseSensitivity);

        // Forward to IntegratedServer for per-player view/simulation distance update
        if (Server::g_integratedServer) {
            Server::g_integratedServer->OnClientSettingsReceived(GetConnectionId(), renderDistance,
                                                                 simulationDistance);
        }
    }

    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    bool ServerConnection::ValidatePacketSize(const std::vector<uint8_t>& payload, size_t expectedMin) {
        if (payload.size() < expectedMin) {
            Log::Warning("[ServerConnection %u] Packet too small: %zu < %zu",
                GetConnectionId(), payload.size(), expectedMin);
            return false;
        }
        return true;
    }

    bool ServerConnection::CheckRateLimit(const std::string& action) {
        auto now = std::chrono::steady_clock::now();
        auto& limit = m_rateLimits[action];
        
        // Reset counter every second
        if (now > limit.resetTime) {
            limit.count = 0;
            limit.resetTime = now + std::chrono::seconds(1);
        }
        
        // Check limit (e.g., 10 actions per second)
        if (limit.count >= 10) {
            Log::Warning("[ServerConnection %u] Rate limit exceeded for %s",
                GetConnectionId(), action.c_str());
            return false;
        }
        
        limit.count++;
        return true;
    }

    // MC's analogue is netty's ReadTimeoutHandler (ServerConnectionListener
    // .java:67) — a watchdog on inbound BYTES, not on any particular packet.
    // m_lastPacketReceivedMs is therefore stamped by every inbound frame (see
    // OnPacketReceived), which is what makes a 30 s window safe; the old 60 s
    // was generous only because almost nothing refreshed it during PLAY.
    //
    // Owner-exempt for the same reason as the keep-alive: MC's integrated
    // server host is outside these timers, and the local client of a stalled
    // integrated server is exactly the peer that cannot answer in time.
    bool ServerConnection::IsTimedOut() const {
        if (IsSingleplayerOwner()) return false;

        const int64_t silentMs = NowMs() - m_lastPacketReceivedMs.load(std::memory_order_relaxed);
        if (m_phase == ConnectionPhase::LOGIN) {
            return silentMs > LOGIN_TIMEOUT_MS;
        }
        if (m_phase == ConnectionPhase::PLAY) {
            return silentMs > READ_TIMEOUT_MS;
        }
        return false;
    }
    
    // Decode packet on I/O thread (override from base class)
    Network::PacketPtr ServerConnection::DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
        using namespace Network;
        
        // Create typed packets based on packet ID and current protocol state
        // This runs on the I/O thread, so we just create the packet object
        // The actual handling happens on the server thread via tick()
        
        PacketReader reader(payload);
        
        switch (static_cast<PacketId>(packetId)) {
            case PacketId::Handshake:
                if (m_phase == ConnectionPhase::HANDSHAKING) {
                    auto packet = std::make_unique<HandshakeC2SPacket>(reader);
                    
                    // CRITICAL: Switch protocol state NOW on I/O thread
                    // This ensures the next packet (LoginStart) is decoded with correct state
                    // This is exactly how Minecraft/Netty handles it
                    if (packet->nextState == static_cast<int32_t>(NextStateWire::LOGIN)) {
                        Log::Debug("[ServerConnection %u] I/O thread switching to LOGIN state", GetConnectionId());
                        m_phase = ConnectionPhase::LOGIN;
                        // Note: Listener will be created on server thread when packet is processed
                    } else if (packet->nextState == static_cast<int32_t>(NextStateWire::STATUS)) {
                        Log::Debug("[ServerConnection %u] I/O thread switching to STATUS state", GetConnectionId());
                        m_phase = ConnectionPhase::STATUS;
                    }
                    
                    return packet;
                }
                break;
                
            case PacketId::LoginStart:
                if (m_phase == ConnectionPhase::LOGIN) {
                    return std::make_unique<LoginStartC2SPacket>(reader);
                }
                break;
                
            // KeepAliveC2S deliberately absent: it is answered inline on the
            // I/O thread by the registry handler below (MC's handleKeepAlive
            // carries no ensureRunningOnSameThread), so it must NOT become a
            // typed packet queued for the tick thread.

            
            case PacketId::UseItemOnC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeUseItemOnC2S(payload);
                    return std::make_unique<Network::Packets::UseItemOnC2SPacketImpl>(std::move(data));
                }
                break;

            case PacketId::UseItem:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeUseItemC2S(payload);
                    return std::make_unique<Network::Packets::UseItemC2SPacketImpl>(data);
                }
                break;

            case PacketId::PlayerAction:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializePlayerActionC2S(payload);
                    return std::make_unique<Network::Packets::PlayerActionC2SPacketImpl>(data);
                }
                break;
            
            case PacketId::ChunkRequestFullC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeChunkRequestFullC2S(payload);
                    return std::make_unique<Network::Packets::ChunkRequestFullC2SPacketImpl>(data.chunkX, data.chunkZ, data.dimensionId);
                }
                break;

            case PacketId::ChunkBatchAckC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeChunkBatchAckC2S(payload);
                    return std::make_unique<Network::Packets::ChunkBatchAckC2SPacketImpl>(data.desiredChunksPerTick);
                }
                break;

            case PacketId::InteractC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeInteractC2S(payload);
                    return std::make_unique<Network::Packets::InteractC2SPacketImpl>(data);
                }
                break;

            case PacketId::PlayerAbilitiesC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializePlayerAbilitiesC2S(payload);
                    return std::make_unique<Network::Packets::PlayerAbilitiesC2SPacketImpl>(data);
                }
                break;

            case PacketId::PlayerPauseC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializePlayerPauseC2S(payload);
                    return std::make_unique<Network::Packets::PlayerPauseC2SPacketImpl>(data);
                }
                break;

#if ENABLE_IMMERSIVE_PORTALS
            case PacketId::PortalTeleportC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializePortalTeleportC2S(payload);
                    return std::make_unique<Network::Packets::PortalTeleportC2SPacketImpl>(data);
                }
                break;
#endif
            case PacketId::FillBlocksC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeFillBlocksC2S(payload);
                    return std::make_unique<Network::Packets::FillBlocksC2SPacketImpl>(data);
                }
                break;

            // ── Formerly legacy-registry packets ───────────────────────────
            // Every one of these used to be handled inline on the network I/O
            // thread. They all reach ServerPlayer / the session / the level, so
            // MC would defer all of them (ServerGamePacketListenerImpl opens
            // every handler with ensureRunningOnSameThread). Decoding them here
            // is what puts them on the server thread.
            case PacketId::BlockActionC2S:
                if (m_phase == ConnectionPhase::PLAY && m_authenticated) {
                    auto data = Network::Serialization::DeserializeBlockActionC2S(payload);
                    return std::make_unique<Network::Packets::BlockActionC2SPacketImpl>(std::move(data));
                }
                break;

            case PacketId::PlayerMoveC2S:
                if (m_phase == ConnectionPhase::PLAY && m_authenticated) {
                    auto data = Network::Serialization::DeserializePlayerMoveC2S(payload);
                    return std::make_unique<Network::Packets::PlayerMoveC2SPacketImpl>(std::move(data));
                }
                break;

            case PacketId::ChatMessageC2S:
                if (m_phase == ConnectionPhase::PLAY && m_authenticated) {
                    auto data = Network::Serialization::DeserializeChatMessageC2S(payload);
                    return std::make_unique<Network::Packets::ChatMessageC2SPacketImpl>(std::move(data));
                }
                break;

            case PacketId::HeldItemChange:
                if (m_phase == ConnectionPhase::PLAY && m_authenticated) {
                    auto data = Network::Serialization::DeserializeHeldItemChangeC2S(payload);
                    return std::make_unique<Network::Packets::HeldItemChangeC2SPacketImpl>(std::move(data));
                }
                break;

            case PacketId::InventoryClickC2S:
                if (m_phase == ConnectionPhase::PLAY && m_authenticated) {
                    auto data = Network::Serialization::DeserializeInventoryClickC2S(payload);
                    return std::make_unique<Network::Packets::InventoryClickC2SPacketImpl>(std::move(data));
                }
                break;

            case PacketId::InventoryCloseC2S:
                if (m_phase == ConnectionPhase::PLAY && m_authenticated) {
                    auto data = Network::Serialization::DeserializeInventoryCloseC2S(payload);
                    return std::make_unique<Network::Packets::InventoryCloseC2SPacketImpl>(std::move(data));
                }
                break;

            case PacketId::ClientConfigC2S:
                // PLAY only, and that split is MC's own: the game-phase
                // handleClientInformation defers
                // (ServerGamePacketListenerImpl.java:1953) because a player
                // exists to update, while the configuration-phase one
                // (ServerConfigurationPacketListenerImpl.java:124) does not,
                // because there is nothing to touch yet. Ours matches — before
                // PLAY this falls through to the legacy handler, which parks
                // the value in m_pendingClientViewDistance for the session that
                // does not exist yet.
                if (m_phase == ConnectionPhase::PLAY) {
                    Network::PacketReader reader(payload);
                    const int renderDistance = std::clamp(static_cast<int>(reader.ReadVarInt()), 2, 32);
                    const bool vsync = reader.ReadByte() != 0;
                    const float mouseSensitivity = reader.ReadFloat();
                    const int simulationDistance = ReadSimulationDistance(reader, renderDistance);
                    return std::make_unique<Network::Packets::ClientConfigC2SPacketImpl>(
                        renderDistance, simulationDistance, vsync, mouseSensitivity);
                }
                break;

            case PacketId::ServerboundAcceptTeleportation: {
                // Deliberately NOT gated on ConnectionPhase::PLAY. The join
                // teleport goes out from OnPlayerJoined, several statements
                // before finalizeLogin flips the phase, so the client's ack can
                // legitimately arrive while this connection still reads as
                // LOGIN. Decoding it here queues it for the server thread,
                // where it is applied in FIFO order — i.e. after LoginStart has
                // finished and the PLAY listener exists. Rejecting it here
                // would leave m_awaitingPositionFromClient stuck set, which silently
                // drops every subsequent movement packet.
                auto data = Network::Serialization::DeserializeServerboundAcceptTeleportation(payload);
                return std::make_unique<Network::Packets::AcceptTeleportationC2SPacketImpl>(data.id);
            }

            case PacketId::PlayerLoadedC2S:
                // MC ServerboundPlayerLoadedPacket: unit codec, nothing to read.
                if (m_phase == ConnectionPhase::PLAY) {
                    return std::make_unique<Network::Packets::PlayerLoadedC2SPacketImpl>();
                }
                break;

            // TODO: Add more packet types as we implement them
            // case PacketId::BlockActionC2S:
            // case PacketId::PlayerMoveC2S:
            // case PacketId::ChatMessageC2S:

            default:
                // For packets we haven't converted yet, return nullptr to fall back to legacy
                return nullptr;
        }
        
        // Unknown or invalid packet for current state
        Log::Warning("[ServerConnection %u] Unexpected packet 0x%02X in state %d", 
                    GetConnectionId(), packetId, static_cast<int>(m_phase.load()));
        return nullptr;
    }

    void ServerConnection::SendPacketIn(Game::DimensionId dimension, uint8_t packetId,
                                        const std::vector<uint8_t>& data) {
        if (dimension != m_outboundDimension) {
            Network::DimensionScopeS2CPacket scope;
            scope.dimensionId = static_cast<int8_t>(Game::DimensionToRaw(dimension));
            SendPacket(static_cast<uint8_t>(Network::PacketId::DimensionScopeS2C),
                       Network::Serialization::Serialize(scope));
            m_outboundDimension = dimension;
        }
        SendPacket(packetId, data);
    }

} // namespace Server