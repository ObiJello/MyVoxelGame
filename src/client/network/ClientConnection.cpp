// File: src/client/network/ClientConnection.cpp
#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "ClientConnection.hpp"
#include "common/world/portal/PortalState.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/block/RedstonePlus.hpp"
#include "client/ClientTickRateManager.hpp"
#include "client/world/HushStillnessState.hpp"
#include "client/world/HushSignalState.hpp"
#include "client/entity/LocalItemCooldowns.hpp"
#include "common/network/packets/game/CooldownS2CPacket.hpp"
#include "client/world/AurelithState.hpp"
#include "common/network/packets/game/HushStillnessS2CPacket.hpp"
#include <chrono>
#include <algorithm>
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include "NetworkClient.hpp"
#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/network/packets/S2CPackets.hpp"  // Ensure packet implementations are available
#include "common/network/packets/HandshakeC2S.hpp"  // Network::kProtocolVersion
#include "../world/ClientChunkManager.hpp"
#include "../entity/RemotePlayerManager.hpp"
#include "ClientPlayerInfo.hpp"
#include "platform/GameDirectory.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/network/packets/game/BlockEntityDataS2CPacket.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientBiomeZoom.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/core/HardwareProfile.hpp"
#include "common/core/ThreadPriority.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "platform/CrashHandler.hpp"

// Chat message callback — set by PlatformMain to route messages to ChatComponent
static std::function<void(const Network::ChatMessageS2CPacket&)> s_chatCallback;
static std::function<void(uint32_t, const std::string&)> s_chatBubbleCallback;
// (gameTime, dayTime, doDaylightCycle) — fires on the client main thread,
// from the typed packet queue. It used to fire on the network I/O thread.
static std::function<void(uint64_t, uint64_t, bool)> s_timeUpdateCallback;
// Teleport callback — set by PlatformMain to snap the local Player on /tp.
// Signature: (x, y, z, yaw, pitch, dx, dy, dz). dx/dy/dz are the
// authoritative post-teleport velocity in blocks/sec, matching the
// packet's deltaMovement field (MC convention). For /tp the server
// passes zero (kills momentum); for portal teleports it passes the
// rotated source velocity (carries momentum through the portal pair).
static std::function<void(double, double, double, float, float,
                          double, double, double)> s_teleportCallback;

void SetTimeUpdateCallback(std::function<void(uint64_t, uint64_t, bool)> callback) {
    s_timeUpdateCallback = std::move(callback);
}

void SetChatMessageCallback(std::function<void(const Network::ChatMessageS2CPacket&)> callback) {
    s_chatCallback = std::move(callback);
}

void SetChatBubbleCallback(std::function<void(uint32_t, const std::string&)> callback) {
    s_chatBubbleCallback = std::move(callback);
}

void SetTeleportCallback(std::function<void(double, double, double, float, float,
                                             double, double, double)> callback) {
    s_teleportCallback = std::move(callback);
}

namespace Client {

    namespace {
        // Chunk packets decode off the network I/O thread. Deserialising one and
        // building its sections (~75 us) on the I/O thread made that thread the
        // client's chunk intake: the batch-rate estimate the client reports to
        // the server (ChunkBatchSizeCalculator) is sized by how fast chunks
        // arrive, so a saved view came in at a few hundred chunks a tick no
        // matter how fast the server loaded it. The I/O thread now only copies
        // the payload and queues the packet in order; these threads decode.
        class ChunkDecodePool {
        public:
            static int ThreadCount() { return Core::HardwareProfile::Get().IsLowEnd() ? 1 : 3; }

            static ChunkDecodePool& Get() {
                // Leaked: the threads live for the process, and nothing they
                // touch outlives it (a job owns its payload and its result).
                static ChunkDecodePool* pool = new ChunkDecodePool();
                return *pool;
            }

            void Submit(std::function<void()> job) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_jobs.push_back(std::move(job));
                }
                m_cv.notify_one();
            }

        private:
            ChunkDecodePool() {
                // Weaker machines: one thread still takes the work off the I/O
                // thread without adding contention.
                for (int i = 0; i < ThreadCount(); ++i) {
                    std::thread([this] { Run(); }).detach();
                }
            }

            void Run() {
                PROFILE_THREAD("ChunkDecode");
                // The player is waiting on these, like the server's chunk loads.
                Core::SetCurrentThreadPriority(Core::ThreadPriorityClass::Elevated);
                Platform::InstallThreadCrashStack();
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] { return !m_jobs.empty(); });
                        job = std::move(m_jobs.front());
                        m_jobs.pop_front();
                    }
                    job();
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::deque<std::function<void()>> m_jobs;   // FIFO: the queue's head decodes first
        };

        struct ChunkDecodeJob {
            std::vector<uint8_t> payload;
            Network::ChunkDataS2CPacket data;
            std::string error;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };

        // A ChunkDataS2C whose decode may still be running. It sits in the
        // incoming queue in arrival order like any packet; applying it waits
        // for its own decode, which — the pool being FIFO and this the oldest
        // chunk still queued — is at most that one chunk's decode time, and
        // counts inside the drain's frame budget and the batch timing.
        class AsyncChunkDataS2CPacket : public Network::IS2CPacket {
        public:
            explicit AsyncChunkDataS2CPacket(std::shared_ptr<ChunkDecodeJob> job) : m_job(std::move(job)) {}

            void apply(Network::IPacketListener& listener) override {
                {
                    PROFILE_ZONE_N("WaitChunkDecode");
                    std::unique_lock<std::mutex> lock(m_job->mutex);
                    m_job->cv.wait(lock, [this] { return m_job->done; });
                }
                if (!m_job->error.empty()) {
                    Log::Error("[ClientConnection] Dropped a chunk packet that failed to decode: %s",
                               m_job->error.c_str());
                    return;
                }
                listener.onChunkDataS2C(m_job->data);
            }

            Network::PacketId getId() const override { return Network::PacketId::ChunkDataS2C; }
            std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }

        private:
            std::shared_ptr<ChunkDecodeJob> m_job;
            std::chrono::steady_clock::time_point m_timestamp = std::chrono::steady_clock::now();
        };
    } // namespace

    int ChunkDecodeThreadCount() { return ChunkDecodePool::ThreadCount(); }


    ClientConnection::ClientConnection(tcp::socket socket, NetworkClient* client)
        : NetworkConnection(std::move(socket))
        , m_client(client)
    {
        // Set connection name to "Client" for clearer logging
        SetName("Client");
        
        // Register packet handlers for server → client packets
        using namespace Network;
        m_packetRegistry.RegisterHandler(PacketId::LoginSuccess,
            [this](const std::vector<uint8_t>& p) { HandleLoginSuccess(p); });
        // MC ClientHandshakePacketListenerImpl.handleCompression: the decoder
        // switches on the instant this packet is handled, and every frame after
        // it carries the extra length VarInt. The packet itself arrived
        // uncompressed, because the server enables its encoder only after
        // writing it.
        // Echo the challenge straight back, on the I/O thread. Nothing else:
        // no game state, no stats, no ping display — see SendKeepAliveResponse.
        m_packetRegistry.RegisterHandler(PacketId::KeepAliveS2C,
            [this](const std::vector<uint8_t>& p) {
                Network::PacketReader reader(p);
                SendKeepAliveResponse(reader.ReadLong());
            });

        // The F3+3 ping probe's echo. Timed here, on the I/O thread, for the
        // same reason KeepAlive is answered here: a main-thread drain would
        // add a whole frame (or a stalled integrated-server tick) to a number
        // that is supposed to measure the socket.
        // The world's engine switches. Atomics, so setting them here on the
        // I/O thread is fine; on the host they are the same globals the
        // integrated server just wrote.
        m_packetRegistry.RegisterHandler(PacketId::ServerPausedS2C,
            [](const std::vector<uint8_t>& p) {
                if (p.empty()) return;
                Client::g_clientTickRate.SetServerPaused(p[0] != 0);
            });
        // The Hush's stillness began / lifted. Two numbers into atomics
        // (HushStillnessState), read by the render thread's HushAtmosphere —
        // the same I/O-thread shape as ServerPausedS2C above.
        m_packetRegistry.RegisterHandler(PacketId::HushStillnessS2C,
            [](const std::vector<uint8_t>& p) {
                const Network::HushStillnessS2CPacket packet =
                    Network::Serialization::DeserializeHushStillnessS2C(p);
                Client::HushStillnessState::OnPacket(packet.active, packet.remainingTicks);
            });
        // The Hush's item signals (tuning-fork ping, arrow burst, echo
        // compass): into HushSignalState behind its mutex; the main thread
        // draws and spawns from there. Same I/O-thread shape as above.
        m_packetRegistry.RegisterHandler(PacketId::HushSignalS2C,
            [](const std::vector<uint8_t>& p) {
                Client::HushSignalState::OnPacket(
                    Network::Serialization::DeserializeHushSignalS2C(p));
            });
        // MC ClientboundCooldownPacket: into the local player's cooldown table
        // behind its lock (LocalItemCooldowns); the HUD reads it.
        m_packetRegistry.RegisterHandler(PacketId::CooldownS2C,
            [](const std::vector<uint8_t>& p) {
                Client::LocalItemCooldowns::OnPacket(
                    Network::Serialization::DeserializeCooldownS2C(p));
            });
        // Aurelith's cities (their quest state) and the flourishes the server
        // has no particles for: into AurelithState behind its mutex; the
        // renderers, the sound and the main thread read copies.
        m_packetRegistry.RegisterHandler(PacketId::AurelithS2C,
            [](const std::vector<uint8_t>& p) {
                Client::AurelithState::OnPacket(
                    Network::Serialization::DeserializeAurelithS2C(p));
            });
        m_packetRegistry.RegisterHandler(PacketId::WorldRulesS2C,
            [](const std::vector<uint8_t>& p) {
                if (p.size() < 2) return;
                Game::Portals::SetImmersiveNetherPortals(p[0] != 0);
                Game::Portals::SetPortalGunAllowed(p[1] != 0);
                // Mirrored vanilla rules (a singleplayer host writes the
                // same values its server already holds).
                if (p.size() >= 4) {
                    Game::Rules::SetBool(Game::Rules::Id::ReducedDebugInfo, p[2] != 0);
                    Game::Rules::SetBool(Game::Rules::Id::ImmediateRespawn, p[3] != 0);
                }
                if (p.size() >= 5) Game::RedstonePlus::SetEnabled(p[4] != 0);
            });
        m_packetRegistry.RegisterHandler(PacketId::PongResponseS2C,
            [this](const std::vector<uint8_t>& p) {
                Network::PacketReader reader(p);
                const int64_t sentMs = static_cast<int64_t>(reader.ReadLong());
                const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                m_pingRttMs.store(static_cast<int32_t>(std::max<int64_t>(0, nowMs - sentMs)),
                                  std::memory_order_relaxed);
            });

        m_packetRegistry.RegisterHandler(PacketId::SetCompression,
            [this](const std::vector<uint8_t>& p) {
                Network::PacketReader reader(p);
                const int threshold = static_cast<int>(reader.ReadVarInt());
                EnableCompression(threshold);
                Log::Info("[ClientConnection] Compression enabled, threshold %d bytes", threshold);
            });
        // That is the whole list. Everything else — chat, time, world spawn,
        // player info, the position snap, block entities, block events — now
        // has a typed representation, is decoded in DecodePacket and applied by
        // ClientPacketHandler on the client main thread. MC has exactly one
        // path (typed packet -> packet.handle(listener)) and so, from here, do
        // we; these two survive only because MC's login-phase listener runs on
        // the Netty thread too.
        //
        // Anything added back here runs on the network I/O thread while the
        // render thread is reading the chunk cache. Don't.
    }

    ClientConnection::~ClientConnection() {
    }

    bool ClientConnection::IsPacketAllowedOutbound(uint8_t packetId) const {
        // MC builds one protocol per phase and swaps the ENCODER with the phase
        // (Connection.setupOutboundProtocol), so the set of sendable packets is
        // whatever that protocol's codec knows. Anything else dies in
        // IdDispatchCodec.encode with "Sending unknown packet". This is the
        // same table, expressed directly.
        //
        // Why it matters beyond tidiness: our render loop starts the moment the
        // socket opens and PlayerController pumps PlayerMoveC2S every tick,
        // regardless of protocol phase. Vanilla cannot do that — LocalPlayer,
        // which is what sends ServerboundMovePlayerPacket (LocalPlayer.java:260),
        // does not exist until ClientboundLoginPacket creates it. Those stray
        // pre-PLAY moves are what got mis-framed across the compression switch
        // and killed a remote join.
        switch (m_phase) {
            case ConnectionPhase::HANDSHAKING:
                // HandshakeProtocols.SERVERBOUND carries exactly one packet.
                return packetId == static_cast<uint8_t>(Network::PacketId::Handshake);

            case ConnectionPhase::LOGIN:
                // LoginProtocols.SERVERBOUND. We have no encryption or cookie
                // packets, so LoginStart is the whole set — plus the client
                // settings, which stand in for MC's configuration phase
                // (ServerboundClientInformationPacket, sent before PLAY; see
                // SendHandshakeAndLogin).
                return packetId == static_cast<uint8_t>(Network::PacketId::LoginStart)
                    || packetId == static_cast<uint8_t>(Network::PacketId::ClientConfigC2S);

            case ConnectionPhase::PLAY:
                // GameProtocols.SERVERBOUND_TEMPLATE — everything gameplay.
                // Handshake and LoginStart are NOT in it; sending either now
                // would be a protocol error in vanilla too.
                return packetId != static_cast<uint8_t>(Network::PacketId::Handshake)
                    && packetId != static_cast<uint8_t>(Network::PacketId::LoginStart);
        }
        return false;
    }

    bool ClientConnection::ShouldDeferPacket(uint8_t packetId) const {
        // Mirror of ServerConnection::ShouldDeferPacket — see there. On this
        // side the phase gate is what keeps SetCompression inline, and that is
        // REQUIRED rather than incidental: the decoder has to switch before the
        // next frame is read off the socket, so deferring it to the main thread
        // would leave the I/O thread parsing compressed frames as plaintext.
        // MC has the same property for the same reason
        // (ClientHandshakePacketListenerImpl.handleCompression is not
        // deferred). SetCompression and LoginSuccess both precede the switch to
        // PLAY, so the phase test covers them.
        if (m_phase != ConnectionPhase::PLAY) {
            return false;
        }
        // KeepAliveS2C answers inline on the I/O thread, mirroring MC's
        // ClientCommonPacketListenerImpl.handleKeepAlive:145, which carries no
        // ensureRunningOnSameThread and replies straight off the Netty thread.
        //
        // This is the client half of what makes a 15 s server-side timeout
        // safe: if the reply had to wait for the once-per-frame main-thread
        // drain, a client whose frames are seconds long — precisely what
        // happens when the shared integrated-server process is stalled — could
        // not answer in time and would be kicked from its own world.
        return packetId != static_cast<uint8_t>(Network::PacketId::Disconnect)
            && packetId != static_cast<uint8_t>(Network::PacketId::KeepAliveS2C)
            && packetId != static_cast<uint8_t>(Network::PacketId::PongResponseS2C);
    }

    void ClientConnection::OnConnected() {
        Log::Info("[ClientConnection] Connected to server");
    }

    void ClientConnection::OnDisconnected() {
        Log::Info("[ClientConnection] Disconnected from server");
        
        if (m_client) {
            m_client->OnConnectionClosed("Connection lost");
        }
    }

    void ClientConnection::OnError(const error_code& error) {
        Log::Error("[ClientConnection] Error: %s", error.message().c_str());
        
        if (m_client) {
            m_client->OnConnectionError(error.message());
        }
    }

    void ClientConnection::OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) {
        // Debug: Log all packet receptions to see if client is receiving anything
        Log::Info("[ClientConnection] PACKET RECEIVED: ID=0x%02X, Size=%zu bytes", packetId, payload.size());
        
        // Add special logging for important packets
        if (packetId == 0x20) {
            Log::Info("[ClientConnection] *** CHUNK DATA PACKET RECEIVED! *** ID=0x20, Size=%zu bytes", payload.size());
        } else if (packetId == 0x06) {
            Log::Info("[ClientConnection] *** LOGIN SUCCESS PACKET RECEIVED! *** ID=0x06, Size=%zu bytes", payload.size());
        }
        
        // Forward to client for statistics
        if (m_client) {
            m_client->OnPacketReceived(packetId, payload);
        }
        
        // Handle packet
        if (!m_packetRegistry.HandlePacket(packetId, payload)) {
            Log::Warning("[ClientConnection] Unhandled packet ID: 0x%02X", packetId);
        }
    }

    void ClientConnection::StartHandshake(const std::string& playerName,
                                          uint8_t playerColor,
                                          const std::string& serverHost,
                                          uint16_t serverPort) {
        m_playerName = playerName;
        m_phase = ConnectionPhase::HANDSHAKING;

        // Send handshake packet
        Network::PacketBuffer buffer;
        buffer.WriteVarInt(Network::kProtocolVersion);
        buffer.WriteString(serverHost); // Server address
        buffer.WriteShort(serverPort); // Server port
        buffer.WriteVarInt(2); // Next state: LOGIN
        SendPacket(static_cast<uint8_t>(Network::PacketId::Handshake), buffer.GetData());

        m_phase = ConnectionPhase::LOGIN;

        // Send login start packet — name + colorId (1 byte). Tail-appending the
        // colorId means an old server that doesn't read it just sees the name and
        // ignores the extra byte; a new server reading from an old client gets
        // remaining()==0 after the name and falls back to Default colour.
        Network::PacketBuffer loginBuffer;
        loginBuffer.WriteString(playerName);
        loginBuffer.WriteByte(playerColor);
        SendPacket(static_cast<uint8_t>(Network::PacketId::LoginStart), loginBuffer.GetData());

        Log::Info("[ClientConnection] Sent handshake and login for player: %s (color id=%u)",
                  playerName.c_str(), static_cast<unsigned>(playerColor));

        // The view and simulation distance go out BEFORE play, as MC's
        // configuration phase sends ClientInformation before the game
        // starts. The server parks them (ServerConnection::HandleClientSettings)
        // and applies them when the session is created, so the first tracking
        // view is already the full one. Sent only after LOGIN SUCCESS, they
        // waited for the server's next tick: a saved world's view started at
        // the 7x7 default and grew to 32 up to a tick later. Login success
        // sends them again (HandleLoginSuccess), which changes nothing when
        // this copy arrived.
        SendClientSettings(Platform::g_gameSettings.GetRenderDistance(),
                           Platform::g_gameSettings.GetSimulationDistance(),
                           Platform::g_gameSettings.GetVSync(),
                           Platform::g_gameSettings.GetMouseSensitivity());
    }

    // ========================================================================
    // PACKET SENDING (CLIENT → SERVER)
    // ========================================================================

    void ClientConnection::SendBlockAction(const Network::BlockActionC2SPacket& packet) {
        Log::Info("[Client] SENDING BlockActionC2S (ID: 0x%02X) - Action: %d, Pos: (%d, %d, %d)",
                  static_cast<uint8_t>(Network::PacketId::BlockActionC2S), 
                  packet.action, packet.worldX, packet.worldY, packet.worldZ);
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::BlockActionC2S), data);
    }

    void ClientConnection::SendPlayerMove(const Network::PlayerMoveC2SPacket& packet) {
        // Only log non-keep-alive player moves to reduce spam
        if (packet.sequenceNumber % 20 == 0) {  // Log every 20th move
            /*Log::Debug("[Client] SENDING PlayerMoveC2S (ID: 0x%02X) - Pos: (%.2f, %.2f, %.2f)",
                      static_cast<uint8_t>(Network::PacketId::PlayerMoveC2S),
                      packet.position.x, packet.position.y, packet.position.z);*/
        }
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerMoveC2S), data);
    }

    void ClientConnection::SendChatMessage(const std::string& message) {
        Network::ChatMessageC2SPacket packet;
        packet.message = message;
        packet.timestamp = static_cast<uint32_t>(std::time(nullptr));
        packet.isCommand = !message.empty() && message[0] == '/';
        
        Log::Info("[Client] SENDING ChatMessageC2S (ID: 0x%02X) - Message: %s",
                  static_cast<uint8_t>(Network::PacketId::ChatMessageC2S), message.c_str());
        
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ChatMessageC2S), data);
    }

    void ClientConnection::SendClientSettings(int renderDistance, int simulationDistance,
                                              bool vsync, float mouseSensitivity) {
        Log::Info("[Client] SENDING ClientConfigC2S (ID: 0x%02X) - RenderDist: %d, SimDist: %d, VSync: %s",
                  static_cast<uint8_t>(Network::PacketId::ClientConfigC2S),
                  renderDistance, simulationDistance, vsync ? "true" : "false");

        Network::PacketBuffer buffer;
        buffer.WriteVarInt(renderDistance);
        buffer.WriteByte(vsync ? 1 : 0);
        buffer.WriteFloat(mouseSensitivity);
        // Appended last so an older server (which stops reading after the
        // float) still accepts the packet.
        buffer.WriteVarInt(simulationDistance);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ClientConfigC2S), buffer.GetData());
    }

    // MAY RUN ON THE NETWORK I/O THREAD (see ShouldDeferPacket). Safe as
    // written because SendPacket is mutex-guarded and posts to the strand —
    // but do NOT add anything here that reads game state, updates a ping
    // display, or writes stats. Keep it to echoing the id back, which is all
    // MC's handleKeepAlive does.
    void ClientConnection::SendKeepAliveResponse(uint64_t id) {
        Network::PacketBuffer buffer;
        buffer.WriteLong(id);
        SendPacket(static_cast<uint8_t>(Network::PacketId::KeepAliveC2S), buffer.GetData());
    }

    void ClientConnection::SendPingRequest() {
        if (m_phase != ConnectionPhase::PLAY) return;
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        Network::PacketBuffer buffer;
        buffer.WriteLong(static_cast<uint64_t>(nowMs));
        SendPacket(static_cast<uint8_t>(Network::PacketId::PingRequestC2S), buffer.GetData());
    }

    int32_t ClientConnection::ConsumePingRtt() {
        return m_pingRttMs.exchange(-1, std::memory_order_relaxed);
    }

    // ========================================================================
    // PACKET HANDLERS (SERVER → CLIENT)
    // ========================================================================

    void ClientConnection::HandleLoginSuccess(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        std::string uuid = reader.ReadString();
        std::string username = reader.ReadString();
        // Trailing field: the world's hashed seed, for the biome zoom. Set
        // before any chunk of the level can arrive.
        if (reader.Remaining() >= 8) {
            ::Client::SetBiomeZoomSeed(static_cast<int64_t>(reader.ReadLong()));
        }
        
        Log::Info("[ClientConnection] LOGIN SUCCESS RECEIVED! User: %s (UUID: %s)", 
            username.c_str(), uuid.c_str());
        
        m_playerId = std::stoul(uuid); // Simple conversion for now
        m_loggedIn = true;
        m_phase = ConnectionPhase::PLAY;

        // Propagate the server-resolved username to the cached local name
        // AND to the global NetworkClient. The server is authoritative on
        // names (auto-assigns "PlayerN" when the client connected without
        // one), so anything keyed by the local-player name — including
        // tab-completion's "self" suggestion — needs the resolved value.
        m_playerName = username;
        if (m_client) {
            m_client->SetPlayerName(username);
        }
        
        // Send client settings with actual render distance from game settings
        SendClientSettings(
            Platform::g_gameSettings.GetRenderDistance(),
            Platform::g_gameSettings.GetSimulationDistance(),
            Platform::g_gameSettings.GetVSync(),
            Platform::g_gameSettings.GetMouseSensitivity()
        );
    }

    void ClientConnection::HandleDisconnect(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        std::string reason = reader.ReadString();
        
        Log::Info("[ClientConnection] Disconnected by server: %s", reason.c_str());
        
        Disconnect();
        
        if (m_client) {
            m_client->OnConnectionClosed(reason);
        }
    }


    void ClientConnection::HandleBlockEntityData(const Network::BlockEntityDataS2CPacket& packet) {
        // Client main thread only — reached from the typed packet queue in
        // DrainIncomingPackets. This ran on the network I/O thread until the
        // packet-threading rework; the assert is the standing proof it does not.
        ASSERT_CLIENT_THREAD();
        if (!g_clientChunkManager) return;
        const auto chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(packet.worldX, packet.worldZ);
        auto* clientChunk = g_clientChunkManager->GetChunk(chunkPos);
        if (!clientChunk || !clientChunk->chunkData) return;

        const auto* type = Game::BlockEntityTypes::ForId(packet.typeId);
        if (!type) {
            Log::Warning("[ClientConnection] BE data for unknown type id %u", packet.typeId);
            return;
        }
        const int lx = packet.worldX - chunkPos.x * 16;
        const int lz = packet.worldZ - chunkPos.z * 16;
        // Read the current block ID from the chunk. The BlockChange packet
        // is sent right before BlockEntityData (same SetBlock call), so the
        // expected block id should already be in the chunk. If it isn't (race
        // with packet ordering), the BE still gets stored — the renderer
        // re-reads the variant from the chunk each frame.
        Game::BlockID blockId = clientChunk->chunkData->GetBlock(lx, packet.worldY, lz);
        auto be = type->Create(glm::ivec3(packet.worldX, packet.worldY, packet.worldZ), blockId);
        if (!packet.dataBlob.empty()) {
            Network::PacketReader r(packet.dataBlob);
            be->Load(r);
        }
        // MC loads an update into the existing entity; this builds a new one,
        // so the old one's client-only state is handed over (a spawner's
        // cage spin).
        if (const Game::BlockEntity* previous =
                clientChunk->chunkData->GetBlockEntity(lx, packet.worldY, lz);
            previous && previous->GetType() == type) {
            be->CarryClientState(*previous);
        }
        const bool ticks = be->NeedsTicking();
        clientChunk->chunkData->SetBlockEntity(lx, packet.worldY, lz, std::move(be));
        if (ticks) g_clientChunkManager->RegisterTickingBlockEntity(
            glm::ivec3(packet.worldX, packet.worldY, packet.worldZ));
    }

    void ClientConnection::HandleBlockEvent(const Network::BlockEntityActionS2CPacket& packet) {
        ASSERT_CLIENT_THREAD();
        if (!g_clientBlockAccess) return;
        g_clientBlockAccess->BlockEvent(glm::ivec3(packet.worldX, packet.worldY, packet.worldZ),
                                        static_cast<Game::BlockID>(packet.blockId),
                                        packet.actionType, packet.actionParam);
    }

    void ClientConnection::HandleBlockEntityRemove(const Network::BlockEntityRemoveS2CPacket& packet) {
        // Client main thread only — reached from the typed packet queue in
        // DrainIncomingPackets. This ran on the network I/O thread until the
        // packet-threading rework; the assert is the standing proof it does not.
        ASSERT_CLIENT_THREAD();
        if (!g_clientChunkManager) return;
        const auto chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(packet.worldX, packet.worldZ);
        auto* clientChunk = g_clientChunkManager->GetChunk(chunkPos);
        if (!clientChunk || !clientChunk->chunkData) return;
        const int lx = packet.worldX - chunkPos.x * 16;
        const int lz = packet.worldZ - chunkPos.z * 16;
        clientChunk->chunkData->RemoveBlockEntity(lx, packet.worldY, lz);
    }

    void ClientConnection::HandleChatMessage(const Network::ChatMessageS2CPacket& packet) {
        // Client main thread only — reached from the typed packet queue in
        // DrainIncomingPackets. This ran on the network I/O thread until the
        // packet-threading rework; the assert is the standing proof it does not.
        ASSERT_CLIENT_THREAD();

        // Flattened text for logging and for the speech bubble, which has no
        // room for styling anyway.
        std::string message;
        for (const auto& seg : packet.segments) message += seg.text;

        const char* positionStr = "";
        switch (packet.position) {
            case 0: positionStr = "[CHAT]"; break;
            case 1: positionStr = "[SYSTEM]"; break;
            case 2: positionStr = "[ACTION]"; break;
        }

        Log::Info("%s %s", positionStr, message.c_str());

        // Add to chat HUD, keeping the styled runs intact so click/hover work.
        if (s_chatCallback) {
            s_chatCallback(packet);
        }

        // Set chat bubble on the remote player (not on self)
        if (s_chatBubbleCallback && packet.senderId != 0) {
            s_chatBubbleCallback(packet.senderId, message);
        }
    }

    void ClientConnection::HandleTimeUpdate(const Network::TimeUpdateS2CPacket& packet) {
        // Client main thread only — reached from the typed packet queue in
        // DrainIncomingPackets. This ran on the network I/O thread until the
        // packet-threading rework; the assert is the standing proof it does not.
        ASSERT_CLIENT_THREAD();
        m_worldAge = packet.worldAge;
        m_timeOfDay = packet.timeOfDay;
        m_doDaylightCycle = packet.doDaylightCycle;

        if (s_timeUpdateCallback) {
            s_timeUpdateCallback(m_worldAge, m_timeOfDay, m_doDaylightCycle);
        }

        Log::Debug("[ClientConnection] Time update: age=%lu, time=%lu, cycle=%d",
            m_worldAge, m_timeOfDay, m_doDaylightCycle ? 1 : 0);
    }

    void ClientConnection::HandleWorldSpawn(const Network::WorldSpawnS2CPacket& packet) {
        // Client main thread only — reached from the typed packet queue in
        // DrainIncomingPackets. This ran on the network I/O thread until the
        // packet-threading rework; the assert is the standing proof it does not.
        ASSERT_CLIENT_THREAD();
        m_spawnPosition.x = static_cast<float>(packet.x);
        m_spawnPosition.y = static_cast<float>(packet.y);
        m_spawnPosition.z = static_cast<float>(packet.z);

        Log::Info("[ClientConnection] World spawn set to (%.0f, %.0f, %.0f)",
            m_spawnPosition.x, m_spawnPosition.y, m_spawnPosition.z);
    }

    void ClientConnection::HandleClientboundPlayerPosition(const Network::ClientboundPlayerPositionPacket& packet) {
        // Client main thread only — reached from the typed packet queue in
        // DrainIncomingPackets. This ran on the network I/O thread until the
        // packet-threading rework; the assert is the standing proof it does not.
        ASSERT_CLIENT_THREAD();

        // MC's client computes absolute values from current player state when relative bits are
        // set. Our /tp only ever sends absolute (relatives == 0), so the snap is straightforward.
        // If we ever support relative teleport, this is where to apply Relative bit logic.
        if (s_teleportCallback) {
            s_teleportCallback(packet.x, packet.y, packet.z,
                               packet.yRot, packet.xRot,
                               packet.dx, packet.dy, packet.dz);
        } else {
            Log::Warning("[ClientConnection] Got teleport packet but no callback registered");
        }

        // Echo the id back so the server's awaiting-teleport gate clears
        // (MC: ServerboundAcceptTeleportationPacket).
        Network::ServerboundAcceptTeleportationPacket ack;
        ack.id = packet.id;
        auto data = Network::Serialization::Serialize(ack);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ServerboundAcceptTeleportation), data);

        Log::Info("[ClientConnection] Teleport id=%d → (%.2f, %.2f, %.2f), acked",
                  packet.id, packet.x, packet.y, packet.z);
    }

    void ClientConnection::HandlePlayerInfo(const Network::PlayerInfoS2CPacket& packet) {
        // Client main thread only — reached from the typed packet queue in
        // DrainIncomingPackets. This ran on the network I/O thread until the
        // packet-threading rework; the assert is the standing proof it does not.
        ASSERT_CLIENT_THREAD();

        // The listed-player table (MC playerInfoMap) keeps everyone, the local
        // player included — the tab list and the spectator menu read it.
        {
            auto& infos = Client::PlayerInfoMap();
            Client::LocalPlayerInfoId() = m_playerId;
            switch (packet.action) {
                case Network::PlayerInfoS2CPacket::Action::ADD: {
                    Client::PlayerInfo& info = infos[packet.playerId];
                    info.playerId = packet.playerId;
                    info.name     = packet.playerName;
                    info.gameMode = packet.gameMode;
                    info.colorId  = packet.colorId;
                    info.latency  = packet.latency;
                    break;
                }
                case Network::PlayerInfoS2CPacket::Action::REMOVE:
                    infos.erase(packet.playerId);
                    break;
                case Network::PlayerInfoS2CPacket::Action::UPDATE_GAME_MODE: {
                    auto it = infos.find(packet.playerId);
                    if (it != infos.end()) it->second.gameMode = packet.gameMode;
                    break;
                }
                case Network::PlayerInfoS2CPacket::Action::UPDATE_LATENCY: {
                    auto it = infos.find(packet.playerId);
                    if (it != infos.end()) it->second.latency = packet.latency;
                    return;   // nothing else on the client reads it
                }
            }
        }

        // Don't track our own player ID in the remote player manager — it's only for OTHER players
        // (matching MC: the local player is in PlayerInfo for tab-list purposes, but we don't render
        // ourselves as a remote entity).
        if (packet.playerId == m_playerId) {
            // LoginSuccess fires BEFORE the server resolves duplicate /
            // empty names (the rename happens in OnPlayerJoined). So
            // for the self entry, the PlayerInfoS2C ADD is the FIRST
            // packet that carries the authoritative name. Sync it into
            // the local name cache + the global NetworkClient so
            // anything name-keyed (e.g. tab-completion's "self"
            // suggestion) sees the resolved value.
            if (packet.action == Network::PlayerInfoS2CPacket::Action::ADD
                && !packet.playerName.empty()) {
                m_playerName = packet.playerName;
                if (m_client) m_client->SetPlayerName(packet.playerName);
            }
            Log::Info("[ClientConnection] PlayerInfo: self entry resolved to '%s' (ID: %u)",
                      packet.playerName.c_str(), packet.playerId);
            return;
        }

        if (!Client::g_remotePlayerManager) return;

        if (packet.action == Network::PlayerInfoS2CPacket::Action::ADD) {
            Client::g_remotePlayerManager->SetPlayerName(packet.playerId, packet.playerName);
            Client::g_remotePlayerManager->SetPlayerColor(
                packet.playerId,
                static_cast<Game::PlayerColorId>(packet.colorId));
            Log::Info("[ClientConnection] PlayerInfo ADD: '%s' (ID: %u, color id=%u)",
                      packet.playerName.c_str(), packet.playerId,
                      static_cast<unsigned>(packet.colorId));
        } else if (packet.action == Network::PlayerInfoS2CPacket::Action::REMOVE) {
            Client::g_remotePlayerManager->RemovePlayer(packet.playerId);
            Client::g_remotePlayerManager->ForgetEquipment(packet.playerId);
            Log::Info("[ClientConnection] PlayerInfo REMOVE: ID %u", packet.playerId);
        }
        if (packet.action == Network::PlayerInfoS2CPacket::Action::ADD ||
            packet.action == Network::PlayerInfoS2CPacket::Action::UPDATE_GAME_MODE) {
            Client::g_remotePlayerManager->SetGameMode(packet.playerId, packet.gameMode);
        }
    }

    // ========================================================================
    // PACKET DECODING (I/O THREAD)
    // ========================================================================
    
    namespace {
        // A PLAY packet a lightweight (bot) connection does not act on.
        // Queued like any typed packet so it never reaches the inline
        // OnPacketReceived path (which logs every packet), and dropped by the
        // bot's drain.
        class DiscardedS2CPacket final : public Network::IS2CPacket {
        public:
            explicit DiscardedS2CPacket(Network::PacketId id)
                : m_id(id), m_time(std::chrono::steady_clock::now()) {}
            Network::PacketId getId() const override { return m_id; }
            std::chrono::steady_clock::time_point getTimestamp() const override { return m_time; }
            void apply(Network::IPacketListener&) override {}
        private:
            Network::PacketId m_id;
            std::chrono::steady_clock::time_point m_time;
        };
    }

    Network::PacketPtr ClientConnection::DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
        using namespace Network;
        using namespace Network::Packets;

        if (m_lightweightDecode.load(std::memory_order_relaxed) && m_phase == ConnectionPhase::PLAY) {
            switch (static_cast<PacketId>(packetId)) {
                case PacketId::ClientboundPlayerPosition:
                case PacketId::ChunkBatchStartS2C:
                case PacketId::ChunkBatchFinishedS2C:
                case PacketId::ChangeDimensionS2C:
                case PacketId::ChatMessageS2C:
                case PacketId::ChunkUnchangedS2C:
                case PacketId::Disconnect:
                case PacketId::KeepAliveS2C:
                case PacketId::PongResponseS2C:
                    break;   // decoded (or left inline) exactly as for a real client
                default:
                    return std::make_unique<DiscardedS2CPacket>(static_cast<PacketId>(packetId));
            }
        }
        
        // Create typed packet on I/O thread based on packet ID
        switch (static_cast<PacketId>(packetId)) {
            case PacketId::ChunkDataS2C: {
                // Decoded on the ChunkDecodePool, applied in arrival order.
                auto job = std::make_shared<ChunkDecodeJob>();
                job->payload = payload;
                if (m_client) {
                    if (auto handler = m_client->GetPacketHandler()) handler->NoteChunkPacketReceived();
                }
                auto decode = [job] {
                    const auto decodeStart = std::chrono::steady_clock::now();
                    try {
                        {
                            PROFILE_ZONE_N("DeserializeChunkData");
                            job->data = Serialization::DeserializeChunkDataS2C(job->payload);
                        }
                        job->data.prebuilt = Client::ClientChunkManager::PrebuildChunk(job->data);
                    } catch (const std::exception& e) {
                        job->error = e.what();
                    } catch (...) {
                        job->error = "unknown exception";
                    }
                    job->data.decodeNanos = std::chrono::duration<double, std::nano>(
                        std::chrono::steady_clock::now() - decodeStart).count();
                    job->payload.clear();
                    job->payload.shrink_to_fit();
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->done = true;
                    }
                    job->cv.notify_all();
                };
                ChunkDecodePool::Get().Submit(std::move(decode));
                return std::make_unique<AsyncChunkDataS2CPacket>(std::move(job));
            }
            
            case PacketId::UnloadChunkS2C: {
                auto data = Serialization::DeserializeUnloadChunkS2C(payload);
                return std::make_unique<UnloadChunkS2CPacketImpl>(std::move(data));
            }

            case PacketId::ChunkUnchangedS2C: {
                auto data = Serialization::DeserializeChunkUnchangedS2C(payload);
                return std::make_unique<ChunkUnchangedS2CPacketImpl>(data);
            }

            case PacketId::LightUpdateS2C: {
                // The layers decode here, on the I/O thread; the main thread
                // only swaps them in.
                auto data = Serialization::DeserializeLightUpdateS2C(payload);
                return std::make_unique<LightUpdateS2CPacketImpl>(std::move(data));
            }

            case PacketId::ChunksBiomesS2C: {
                auto data = Serialization::DeserializeChunksBiomesS2C(payload);
                return std::make_unique<ChunksBiomesS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::BlockChangeS2C: {
                auto data = Serialization::DeserializeBlockChangeS2C(payload);
                return std::make_unique<BlockChangeS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::ClientboundSectionBlocksUpdate: {
                auto data = Serialization::DeserializeClientboundSectionBlocksUpdate(payload);
                return std::make_unique<ClientboundSectionBlocksUpdateS2CPacketImpl>(std::move(data));
            }
            case PacketId::MultiBlockChangeS2C: {
                auto data = Serialization::DeserializeMultiBlockChangeS2C(payload);
                return std::make_unique<MultiBlockChangeS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::PlayerUpdateS2C: {
                auto data = Serialization::DeserializePlayerUpdateS2C(payload);
                return std::make_unique<PlayerUpdateS2CPacketImpl>(std::move(data));
            }

            case PacketId::EntityDestroy: {
                auto data = Serialization::DeserializeRemoveEntitiesS2C(payload);
                return std::make_unique<RemoveEntitiesS2CPacketImpl>(std::move(data));
            }

            case PacketId::ItemEntitySpawnS2C: {
                auto data = Serialization::DeserializeItemEntitySpawnS2C(payload);
                return std::make_unique<ItemEntitySpawnS2CPacketImpl>(std::move(data));
            }

            case PacketId::ItemEntityMoveS2C: {
                auto data = Serialization::DeserializeItemEntityMoveS2C(payload);
                return std::make_unique<ItemEntityMoveS2CPacketImpl>(std::move(data));
            }

            case PacketId::TakeItemEntityS2C: {
                auto data = Serialization::DeserializeTakeItemEntityS2C(payload);
                return std::make_unique<TakeItemEntityS2CPacketImpl>(std::move(data));
            }

            case PacketId::XpOrbSpawnS2C: {
                auto data = Serialization::DeserializeXpOrbSpawnS2C(payload);
                return std::make_unique<XpOrbSpawnS2CPacketImpl>(std::move(data));
            }

            case PacketId::XpOrbMoveS2C: {
                auto data = Serialization::DeserializeXpOrbMoveS2C(payload);
                return std::make_unique<XpOrbMoveS2CPacketImpl>(std::move(data));
            }

            case PacketId::SetExperienceS2C: {
                auto data = Serialization::DeserializeSetExperienceS2C(payload);
                return std::make_unique<SetExperienceS2CPacketImpl>(std::move(data));
            }

            case PacketId::AddEntityS2C: {
                auto data = Serialization::DeserializeAddEntityS2C(payload);
                return std::make_unique<AddEntityS2CPacketImpl>(std::move(data));
            }

            case PacketId::MoveEntityS2C: {
                auto data = Serialization::DeserializeMoveEntityS2C(payload);
                return std::make_unique<MoveEntityS2CPacketImpl>(std::move(data));
            }

            case PacketId::EntityPositionSyncS2C: {
                auto data = Serialization::DeserializeEntityPositionSyncS2C(payload);
                return std::make_unique<EntityPositionSyncS2CPacketImpl>(std::move(data));
            }

            case PacketId::EntityPositionSyncBatchS2C: {
                auto data = Serialization::DeserializeEntityPositionSyncBatchS2C(payload);
                return std::make_unique<EntityPositionSyncBatchS2CPacketImpl>(std::move(data));
            }

            case PacketId::SetEntityMotionS2C: {
                auto data = Serialization::DeserializeSetEntityMotionS2C(payload);
                return std::make_unique<SetEntityMotionS2CPacketImpl>(std::move(data));
            }

            case PacketId::SetEntityDataS2C: {
                auto data = Serialization::DeserializeSetEntityDataS2C(payload);
                return std::make_unique<SetEntityDataS2CPacketImpl>(std::move(data));
            }

            case PacketId::HurtAnimationS2C: {
                auto data = Serialization::DeserializeHurtAnimationS2C(payload);
                return std::make_unique<HurtAnimationS2CPacketImpl>(std::move(data));
            }
            case PacketId::PlayerSleepS2C: {
                auto data = Serialization::DeserializePlayerSleepS2C(payload);
                return std::make_unique<PlayerSleepS2CPacketImpl>(std::move(data));
            }
            case PacketId::PlayerMountS2C: {
                auto data = Serialization::DeserializePlayerMountS2C(payload);
                return std::make_unique<PlayerMountS2CPacketImpl>(data);
            }
            case PacketId::SetPassengersS2C: {
                auto data = Serialization::DeserializeSetPassengersS2C(payload);
                return std::make_unique<SetPassengersS2CPacketImpl>(std::move(data));
            }
            case PacketId::MoveVehicleS2C: {
                auto data = Serialization::DeserializeMoveVehicleS2C(payload);
                return std::make_unique<MoveVehicleS2CPacketImpl>(data);
            }
            case PacketId::MountScreenOpenS2C: {
                auto data = Serialization::DeserializeMountScreenOpenS2C(payload);
                return std::make_unique<MountScreenOpenS2CPacketImpl>(data);
            }
            case PacketId::UpdateAttributesS2C: {
                auto data = Serialization::DeserializeUpdateAttributesS2C(payload);
                return std::make_unique<UpdateAttributesS2CPacketImpl>(std::move(data));
            }
            case PacketId::VehicleDataS2C: {
                auto data = Serialization::DeserializeVehicleDataS2C(payload);
                return std::make_unique<VehicleDataS2CPacketImpl>(data);
            }
            case PacketId::PlayerSwingS2C: {
                auto data = Serialization::DeserializePlayerSwingS2C(payload);
                return std::make_unique<PlayerSwingS2CPacketImpl>(data);
            }
            case PacketId::ShoulderParrotsS2C: {
                auto data = Serialization::DeserializeShoulderParrotsS2C(payload);
                return std::make_unique<ShoulderParrotsS2CPacketImpl>(data);
            }
            case PacketId::OpenSignEditorS2C: {
                auto data = Serialization::DeserializeOpenSignEditorS2C(payload);
                return std::make_unique<OpenSignEditorS2CPacketImpl>(std::move(data));
            }
            case PacketId::OpenBookS2C: {
                auto data = Serialization::DeserializeOpenBookS2C(payload);
                return std::make_unique<OpenBookS2CPacketImpl>(data);
            }
            case PacketId::MerchantOffersS2C: {
                auto data = Serialization::DeserializeMerchantOffersS2C(payload);
                return std::make_unique<MerchantOffersS2CPacketImpl>(std::move(data));
            }
            case PacketId::SetCameraS2C: {
                auto data = Serialization::DeserializeSetCameraS2C(payload);
                return std::make_unique<SetCameraS2CPacketImpl>(data);
            }
            case PacketId::UpdateMobEffectS2C: {
                auto data = Serialization::DeserializeUpdateMobEffectS2C(payload);
                return std::make_unique<UpdateMobEffectS2CPacketImpl>(std::move(data));
            }
            case PacketId::RemoveMobEffectS2C: {
                auto data = Serialization::DeserializeRemoveMobEffectS2C(payload);
                return std::make_unique<RemoveMobEffectS2CPacketImpl>(std::move(data));
            }
            case PacketId::SoundS2C: {
                auto data = Serialization::DeserializeSoundS2C(payload);
                return std::make_unique<SoundS2CPacketImpl>(std::move(data));
            }
            case PacketId::SoundEntityS2C: {
                auto data = Serialization::DeserializeSoundEntityS2C(payload);
                return std::make_unique<SoundEntityS2CPacketImpl>(std::move(data));
            }
            case PacketId::LevelEventS2C: {
                auto data = Serialization::DeserializeLevelEventS2C(payload);
                return std::make_unique<LevelEventS2CPacketImpl>(data);
            }
            case PacketId::JukeboxSongS2C: {
                auto data = Serialization::DeserializeJukeboxSongS2C(payload);
                return std::make_unique<JukeboxSongS2CPacketImpl>(data);
            }
            case PacketId::SelfParticleStateS2C: {
                auto data = Serialization::DeserializeSelfParticleStateS2C(payload);
                return std::make_unique<SelfParticleStateS2CPacketImpl>(data);
            }
            case PacketId::LevelParticlesS2C: {
                auto data = Serialization::DeserializeLevelParticlesS2C(payload);
                return std::make_unique<LevelParticlesS2CPacketImpl>(std::move(data));
            }
            case PacketId::ControlS2C: {
                auto data = Serialization::DeserializeControlS2C(payload);
                return std::make_unique<ControlS2CPacketImpl>(std::move(data));
            }
            case PacketId::ControlInputS2C: {
                auto data = Serialization::DeserializeControlInput(payload);
                return std::make_unique<ControlInputS2CPacketImpl>(std::move(data));
            }
            case PacketId::ControlViewS2C: {
                auto data = Serialization::DeserializeControlView(payload);
                return std::make_unique<ControlViewS2CPacketImpl>(std::move(data));
            }
            case PacketId::MorphHeldS2C: {
                auto data = Serialization::DeserializeMorphHeldS2C(payload);
                return std::make_unique<MorphHeldS2CPacketImpl>(std::move(data));
            }
            case PacketId::MorphPickupS2C: {
                auto data = Serialization::DeserializeMorphPickupS2C(payload);
                return std::make_unique<MorphPickupS2CPacketImpl>(std::move(data));
            }
            case PacketId::EntityEventS2C: {
                auto data = Serialization::DeserializeEntityEventS2C(payload);
                return std::make_unique<EntityEventS2CPacketImpl>(std::move(data));
            }

            case PacketId::TickingStateS2C: {
                auto data = Serialization::DeserializeTickingStateS2C(payload);
                return std::make_unique<TickingStateS2CPacketImpl>(std::move(data));
            }

            case PacketId::TickingStepS2C: {
                auto data = Serialization::DeserializeTickingStepS2C(payload);
                return std::make_unique<TickingStepS2CPacketImpl>(std::move(data));
            }
            case PacketId::ChangeDimensionS2C: {
                auto data = Serialization::DeserializeChangeDimensionS2C(payload);
                return std::make_unique<ChangeDimensionS2CPacketImpl>(std::move(data));
            }
            // MC ClientboundGameEventPacket (the weather events) — typed, so
            // it applies on the main thread in order with the dimension
            // change that resets the client's weather.
            case PacketId::WeatherChange: {
                auto data = Serialization::DeserializeGameEventS2C(payload);
                return std::make_unique<GameEventS2CPacketImpl>(std::move(data));
            }

            case PacketId::ExplodeS2C: {
                auto data = Serialization::DeserializeExplodeS2C(payload);
                return std::make_unique<ExplodeS2CPacketImpl>(std::move(data));
            }

            case PacketId::BossEventS2C: {
                auto data = Serialization::DeserializeBossEventS2C(payload);
                return std::make_unique<BossEventS2CPacketImpl>(std::move(data));
            }

            case PacketId::EndCrystalBeamS2C: {
                auto data = Serialization::DeserializeEndCrystalBeamS2C(payload);
                return std::make_unique<EndCrystalBeamS2CPacketImpl>(std::move(data));
            }
            case PacketId::ArmorStandDataS2C: {
                auto data = Serialization::DeserializeArmorStandDataS2C(payload);
                return std::make_unique<ArmorStandDataS2CPacketImpl>(std::move(data));
            }
            case PacketId::FishingHookDataS2C: {
                auto data = Serialization::DeserializeFishingHookDataS2C(payload);
                return std::make_unique<FishingHookDataS2CPacketImpl>(data);
            }
            case PacketId::ItemFrameDataS2C: {
                auto data = Serialization::DeserializeItemFrameDataS2C(payload);
                return std::make_unique<ItemFrameDataS2CPacketImpl>(std::move(data));
            }
            case PacketId::MapItemDataS2C: {
                auto data = Serialization::DeserializeMapItemDataS2C(payload);
                return std::make_unique<MapItemDataS2CPacketImpl>(std::move(data));
            }
            case PacketId::SetEntityLinkS2C: {
                auto data = Serialization::DeserializeSetEntityLinkS2C(payload);
                return std::make_unique<SetEntityLinkS2CPacketImpl>(data);
            }
            case PacketId::FireworkRocketDataS2C: {
                auto data = Serialization::DeserializeFireworkRocketDataS2C(payload);
                return std::make_unique<FireworkRocketDataS2CPacketImpl>(std::move(data));
            }
            case PacketId::BodyArmorS2C: {
                auto data = Serialization::DeserializeBodyArmorS2C(payload);
                return std::make_unique<BodyArmorS2CPacketImpl>(std::move(data));
            }

            case PacketId::Disconnect: {
                PacketReader reader(payload);
                std::string reason = reader.ReadString();
                return std::make_unique<DisconnectPacketImpl>(std::move(reason));
            }

            // ── Formerly legacy-registry packets ───────────────────────────
            // Each of these reaches client world state — the chunk cache, the
            // remote player list, the local player's position — so MC's
            // equivalents in ClientPacketListener all call
            // ensureRunningOnSameThread. Decoding them here is what puts them
            // on the client main thread instead of the network I/O thread.
            case PacketId::ChatMessageS2C: {
                auto data = Network::Serialization::DeserializeChatMessageS2C(payload);
                return std::make_unique<Network::Packets::ChatMessageS2CPacketImpl>(std::move(data));
            }

            case PacketId::TimeUpdate: {
                auto data = Network::Serialization::DeserializeTimeUpdateS2C(payload);
                return std::make_unique<Network::Packets::TimeUpdateS2CPacketImpl>(data);
            }

            case PacketId::WorldSpawn: {
                auto data = Network::Serialization::DeserializeWorldSpawnS2C(payload);
                return std::make_unique<Network::Packets::WorldSpawnS2CPacketImpl>(data);
            }

            case PacketId::PlayerInfoS2C: {
                auto data = Network::Serialization::DeserializePlayerInfoS2C(payload);
                return std::make_unique<Network::Packets::PlayerInfoS2CPacketImpl>(std::move(data));
            }

            case PacketId::ClientboundPlayerPosition: {
                auto data = Network::Serialization::DeserializeClientboundPlayerPosition(payload);
                return std::make_unique<Network::Packets::ClientboundPlayerPositionPacketImpl>(data);
            }

            case PacketId::BlockEntityDataS2C: {
                auto data = Network::Serialization::DeserializeBlockEntityDataS2C(payload);
                return std::make_unique<Network::Packets::BlockEntityDataS2CPacketImpl>(std::move(data));
            }

            case PacketId::BlockEntityRemoveS2C: {
                auto data = Network::Serialization::DeserializeBlockEntityRemoveS2C(payload);
                return std::make_unique<Network::Packets::BlockEntityRemoveS2CPacketImpl>(data);
            }

            case PacketId::BlockEntityActionS2C: {
                auto data = Network::Serialization::DeserializeBlockEntityActionS2C(payload);
                return std::make_unique<Network::Packets::BlockEntityActionS2CPacketImpl>(data);
            }
            
            // KeepAliveS2C deliberately absent. DecodePacket returning a typed
            // packet ALWAYS queues it for the main thread — ShouldDeferPacket
            // is only consulted when decode returns null — so leaving a case
            // here would keep the reply behind the once-per-frame drain no
            // matter what ShouldDeferPacket says. The raw registry handler
            // installed in the constructor answers it on the I/O thread
            // instead, which is what MC does
            // (ClientCommonPacketListenerImpl.handleKeepAlive:145).

            case PacketId::ChunkBatchStartS2C: {
                return std::make_unique<ChunkBatchStartS2CPacketImpl>();
            }

            case PacketId::ChunkBatchFinishedS2C: {
                auto data = Serialization::DeserializeChunkBatchFinishedS2C(payload);
                return std::make_unique<ChunkBatchFinishedS2CPacketImpl>(data.batchSize, data.serverSendMicros);
            }

            case PacketId::HotbarSyncS2C: {
                auto data = Serialization::DeserializeHotbarSyncS2C(payload);
                return std::make_unique<HotbarSyncS2CPacketImpl>(std::move(data));
            }

            case PacketId::SetChunkCacheRadiusS2C: {
                auto data = Serialization::DeserializeSetChunkCacheRadiusS2C(payload);
                return std::make_unique<SetChunkCacheRadiusS2CPacketImpl>(data.viewDistance);
            }

            case PacketId::CommandsS2C: {
                auto data = Serialization::DeserializeCommandsS2C(payload);
                return std::make_unique<CommandsS2CPacketImpl>(std::move(data));
            }

            case PacketId::WorldgenIdsS2C:
                return std::make_unique<WorldgenIdsS2CPacketImpl>(
                    Serialization::DeserializeWorldgenIdsS2C(payload));

            case PacketId::InventoryFullS2C: {
                auto data = Serialization::DeserializeInventoryFullS2C(payload);
                return std::make_unique<InventoryFullS2CPacketImpl>(std::move(data));
            }

            case PacketId::InventorySetSlotS2C: {
                auto data = Serialization::DeserializeInventorySetSlotS2C(payload);
                return std::make_unique<InventorySetSlotS2CPacketImpl>(data);
            }

            case PacketId::InventorySetCarriedS2C: {
                auto data = Serialization::DeserializeInventorySetCarriedS2C(payload);
                return std::make_unique<InventorySetCarriedS2CPacketImpl>(data);
            }

            case PacketId::SetHeldSlotS2C: {
                auto data = Serialization::DeserializeSetHeldSlotS2C(payload);
                return std::make_unique<SetHeldSlotS2CPacketImpl>(data);
            }

            case PacketId::ContainerSetDataS2C: {
                auto data = Serialization::DeserializeContainerSetDataS2C(payload);
                return std::make_unique<ContainerSetDataS2CPacketImpl>(std::move(data));
            }
            case PacketId::OpenScreenS2C: {
                auto data = Serialization::DeserializeOpenScreenS2C(payload);
                return std::make_unique<OpenScreenS2CPacketImpl>(std::move(data));
            }

            case PacketId::SetHealthS2C: {
                auto data = Serialization::DeserializeSetHealthS2C(payload);
                return std::make_unique<SetHealthS2CPacketImpl>(data);
            }

            case PacketId::BlockChangedAckS2C: {
                auto data = Serialization::DeserializeBlockChangedAckS2C(payload);
                return std::make_unique<BlockChangedAckS2CPacketImpl>(data);
            }

            case PacketId::PlayerAbilities: {
                auto data = Serialization::DeserializePlayerAbilitiesS2C(payload);
                return std::make_unique<PlayerAbilitiesS2CPacketImpl>(data);
            }

            case PacketId::DimensionScopeS2C: {
                auto data = Serialization::DeserializeDimensionScopeS2C(payload);
                return std::make_unique<DimensionScopeS2CPacketImpl>(data);
            }

#if ENABLE_IMMERSIVE_PORTALS
            case PacketId::ImmersivePortalSyncS2C: {
                auto data = Serialization::DeserializeImmersivePortalSyncS2C(payload);
                return std::make_unique<ImmersivePortalSyncS2CPacketImpl>(std::move(data));
            }

            case PacketId::ImmersivePortalRemoveS2C: {
                auto data = Serialization::DeserializeImmersivePortalRemoveS2C(payload);
                return std::make_unique<ImmersivePortalRemoveS2CPacketImpl>(data);
            }
#endif
            case PacketId::AoRegionsS2C: {
                auto data = Serialization::DeserializeAoRegionsS2C(payload);
                return std::make_unique<AoRegionsS2CPacketImpl>(std::move(data));
            }

#if ENABLE_PORTAL_GUN
            case PacketId::PortalSetS2C: {
                auto data = Serialization::DeserializePortalSetS2C(payload);
                return std::make_unique<PortalSetS2CPacketImpl>(data);
            }

            case PacketId::PortalRemoveS2C: {
                auto data = Serialization::DeserializePortalRemoveS2C(payload);
                return std::make_unique<PortalRemoveS2CPacketImpl>(data);
            }

            case PacketId::PortalTeleportFlashS2C: {
                auto data = Serialization::DeserializePortalTeleportFlashS2C(payload);
                return std::make_unique<PortalTeleportFlashS2CPacketImpl>(data);
            }

            case PacketId::PortalFizzleS2C: {
                auto data = Serialization::DeserializePortalFizzleS2C(payload);
                return std::make_unique<PortalFizzleS2CPacketImpl>(data);
            }
#endif

            default:
                // Return nullptr for unhandled packets - will fall back to legacy OnPacketReceived
                return nullptr;
        }
    }
    
    // ========================================================================
    // MAIN THREAD PACKET PROCESSING
    // ========================================================================
    
    void ClientConnection::DrainIncomingPackets() {
        PROFILE_ZONE;
        if (!m_client) return;

        auto handler = m_client->GetPacketHandler();
        if (!handler) return;

        // Drain ALL queued packets — matches Minecraft's PacketProcessor.processQueuedPackets().
        // No budget check needed: the client tick (20 TPS) naturally limits how many packets
        // accumulate per drain, and the server's ChunkBatchSizeCalculator + back-pressure
        // limits chunk data throughput to ~7ms per tick.
        // Per-frame budget: a burst of chunk packets (a saved area arriving
        // at ~1,300 chunks/s) used to be applied in whatever frame it landed,
        // 20-30 ms hitches. Order is preserved: what is not applied now is
        // still first in the queue next frame.
        const auto drainStart = std::chrono::steady_clock::now();
        constexpr auto kDrainBudget = std::chrono::milliseconds(6);
        size_t applied = 0;
        Network::IncomingPacket packet;
        while (true) {
            if (applied != 0 && (applied % 8) == 0 &&
                std::chrono::steady_clock::now() - drainStart > kDrainBudget) {
                break;
            }
            if (!TryPopIncoming(packet)) break;
            ++applied;
            // The globals follow the packet: a chunk for the Nether is
            // applied with the Nether level bound, whatever level the
            // player stands in. The scope packet's own handler changes the
            // packet dimension, so this is re-evaluated per packet.
            Client::ClientLevels::BindForPacket();
            try {
                // Undecoded packet deferred off the I/O thread — run its legacy
                // registry handler here, on the client main thread. Disappears
                // once every S2C packet has a typed representation.
                if (auto* raw = dynamic_cast<Network::RawPayloadPacket*>(packet.packet.get())) {
                    PROFILE_ZONE_N("ApplyDeferredPacket");
                    if (!m_packetRegistry.HandlePacket(raw->rawId(), raw->payload())) {
                        Log::Warning("[ClientConnection] Unhandled deferred packet ID 0x%02X",
                                     raw->rawId());
                    }
                } else if (auto* s2cPacket = dynamic_cast<Network::IS2CPacket*>(packet.packet.get())) {
                    PROFILE_ZONE_N("ApplyPacket");
                    handler->SetPacketReceivedAt(s2cPacket->getTimestamp());
                    s2cPacket->apply(*handler);
                    if (s2cPacket->getId() == Network::PacketId::ChunkDataS2C) handler->NoteChunkPacketApplied();
                }
            } catch (const std::exception& e) {
                Log::Error("[ClientConnection] Exception applying packet: %s", e.what());
            }
        }
        // Gameplay and rendering read the ACTIVE level; leave the globals on it.
        Client::ClientLevels::BindActive();
    }

} // namespace Client