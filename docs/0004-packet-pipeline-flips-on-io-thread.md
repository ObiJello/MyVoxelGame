ADR-0004: Packet Pipeline Flips on I/O Thread

Status

Accepted

Context

The Minecraft protocol requires mid-connection changes to packet processing:
- Compression: After SetCompression packet, all subsequent packets use zlib compression. Format: outer VarInt length, VarInt data length (0 = uncompressed, otherwise zlib payload of that size), then the payload data
- Encryption (future): After encryption handshake, all data is AES encrypted
- Protocol State: After Handshake, different packet IDs become valid

These "pipeline flips" must happen atomically with precise timing to ensure client and server stay synchronized. The critical challenge is when and where to perform these changes:
- Too early: Server processes compressed packets before client starts sending them
- Too late: Server tries to read uncompressed packet data as compressed
- Wrong thread: Race conditions between I/O operations and pipeline changes

We need a solution that guarantees atomic pipeline changes without race conditions.

Decision

All packet pipeline modifications (compression, encryption, protocol state changes) will be performed immediately on the I/O thread as part of the packet processing chain. This mirrors Minecraft
Java Edition's Netty pipeline modification pattern.

Rationale

Minecraft Java Edition Pattern

In Minecraft's Netty implementation:
- ChannelPipeline: Chain of handlers that transform raw bytes ↔ packet objects
- Pipeline Modification: channel.pipeline().addBefore("decoder", "decompressor", new Decompressor())
- Atomic Changes: Pipeline modifications happen on the EventLoop (I/O thread)
- Immediate Effect: Next packet processed through new pipeline configuration


    // Minecraft ServerLoginNetworkHandler
    public void handleLoginStart(ServerboundHelloPacket packet) {
        // ... authentication logic

        // Pipeline flip happens immediately on Netty I/O thread
        if (compressionThreshold >= 0) {
            this.connection.setupCompression(compressionThreshold);
            // Next inbound packet will be decompressed
        }

        // Send response
        this.connection.send(new ClientboundLoginCompressionPacket(compressionThreshold));
        // Next outbound packet will be compressed
    }

Our Boost.Asio Equivalent

    // In LoginPacketListener::onLoginStart() - runs on I/O thread
    void LoginPacketListener::onLoginStart(const LoginStartC2SPacket& packet) {
        // Authenticate player
        uint32_t playerId = AuthenticatePlayer(packet.name);

        // Send SetCompression packet  
        m_connection.SendSetCompression(256);

        // CRITICAL: Pipeline flip happens immediately on I/O thread
        m_connection.enableCompression(256);
        // All subsequent packets will be compressed/decompressed

        // Send LoginSuccess (will be compressed)
        m_connection.SendLoginSuccess(playerId, packet.name);
    }

Atomicity Guarantees

Since all operations happen on the same I/O thread via strand:
1. Send SetCompression: Queued for async_write
2. Enable Compression: Pipeline state changed immediately
3. Send LoginSuccess: Uses new compression pipeline
4. Next Inbound Read: Uses compression for decode

No race conditions possible - all operations serialized on single thread.

Implementation Architecture

Compression Pipeline Flip

    // In ServerConnection (runs on I/O thread via strand)
    void ServerConnection::enableCompression(int threshold) {
        // This method called immediately after sending SetCompression

        // Update inbound packet processing
        m_compressionThreshold = threshold;
        m_compressionEnabled = true;

        // Update outbound packet processing
        m_outboundCompression = true;

        Log::Debug("Compression enabled with threshold %d", threshold);

        // Next async_read will use decompression
        // Next async_write will use compression
    }
    
    // In packet reading chain
    void ServerConnection::ProcessInboundPacket(const std::vector<uint8_t>& rawData) {
        std::vector<uint8_t> packetData;

        if (m_compressionEnabled) {
            // Decompress using pipeline state set during enableCompression()
            packetData = DecompressPacket(rawData);
        } else {
            packetData = rawData;
        }

        // Decode packet normally
        DecodeAndEnqueuePacket(packetData);
    }

Protocol State Flip

    // In HandshakePacketListener::onHandshake() - runs on I/O thread
    void HandshakePacketListener::onHandshake(const HandshakeC2SPacket& packet) {
        if (packet.nextState == 2) { // LOGIN
            // Protocol flip happens immediately
            m_connection.setProtocolState(ProtocolState::LOGIN);
            // Next packet IDs interpreted using LOGIN registry
        }

        // No race condition: state change and next packet processing
        // both happen on same I/O thread
    }
    
    void ServerConnection::setProtocolState(ProtocolState state) {
        // Swap packet listener immediately
        switch (state) {
            case ProtocolState::LOGIN:
                m_listener = std::make_unique<LoginPacketListener>(*this);
                break;
            case ProtocolState::PLAY:
                m_listener = std::make_unique<PlayPacketListener>(*this);
                break;
        }

        m_protocolState = state;
        Log::Debug("Protocol state changed to %d", static_cast<int>(state));
    }

Future Encryption Pipeline

    // Future implementation for encryption
    void ServerConnection::enableEncryption(const std::vector<uint8_t>& sharedSecret) {
        // Initialize AES/CFB8 cipher
        m_encryptCipher = InitializeAES_CFB8(sharedSecret);
        m_decryptCipher = InitializeAES_CFB8(sharedSecret);

        m_encryptionEnabled = true;

        // All subsequent I/O operations will encrypt/decrypt
    }
    
    void ServerConnection::ProcessInboundData(const std::vector<uint8_t>& encryptedData) {
        std::vector<uint8_t> decryptedData;

        if (m_encryptionEnabled) {
            decryptedData = m_decryptCipher->Decrypt(encryptedData);
        } else {
            decryptedData = encryptedData;
        }

        // Continue with compression/decompression pipeline
        ProcessInboundPacket(decryptedData);
    }

Thread Safety and Ordering

Single-Threaded Pipeline Modification

All pipeline changes happen on I/O thread via connection strand:
- No Locks Needed: Strand ensures single-threaded execution per connection
- Atomic Visibility: Pipeline state changes visible immediately to subsequent operations
- Ordered Operations: async_write completion → pipeline change → next async_read

Interaction with Server Thread

    // Server thread never directly modifies pipeline
    void ServerConnection::tick() {
        // Server thread only processes decoded packets
        auto packets = m_inboundPackets.DrainAll();
        for (auto& packet : packets) {
            packet->apply(*m_listener); // Uses current listener set by I/O thread
        }

        // Server thread never calls enableCompression() or setProtocolState()
    }

Pipeline State Synchronization

    class ServerConnection {
        private:
            // Pipeline state (modified only on I/O thread)
            std::atomic<bool> m_compressionEnabled{false};
            std::atomic<bool> m_encryptionEnabled{false};
            std::atomic<ProtocolState> m_protocolState{ProtocolState::HANDSHAKING};

              // Accessed only on I/O thread (no atomics needed)
            int m_compressionThreshold = -1;
            std::unique_ptr<AESCipher> m_encryptCipher;
            std::unique_ptr<AESCipher> m_decryptCipher;

            // Connection strand ensures single-threaded access
            boost::asio::io_context::strand m_strand;
    };

Timing Precision

Compression Activation Sequence

Client                          Server
|                              |
|---> HandshakeC2S ---------> |
|                              |--- setProtocolState(LOGIN)
|                              |
|<--- SetCompression(256) ----|
|                              |--- enableCompression(256)
|                              |
|==== LoginStartC2S ======> |  (compressed)
(client compresses)       |
|                              |--- decompress & process
|<==== LoginSuccessS2C ======|  (compressed)
(client decompresses)     |

Critical Timing Points

1. SetCompression sent: Server queues compressed packet for transmission
2. enableCompression() called: Server pipeline now expects compressed input
3. LoginStart received: First compressed packet from client
4. No gap: Zero packets processed with wrong compression state

Error Handling

Pipeline Mismatch Detection

    void ServerConnection::ProcessInboundPacket(const std::vector<uint8_t>& data) {
        try {
            if (m_compressionEnabled) {
                auto decompressed = DecompressPacket(data);
                DecodePacket(decompressed);
            } else {
                DecodePacket(data);
            }
        } catch (const CompressionException& e) {
            Log::Error("Compression error - pipeline mismatch? %s", e.what());
            SendDisconnect("Protocol error");
            Close();
        } catch (const PacketDecodeException& e) {
            Log::Error("Packet decode error - wrong protocol state? %s", e.what());
            SendDisconnect("Protocol error");
            Close();
        }
    }

State Validation

    bool ServerConnection::ValidatePacketForCurrentState(uint8_t packetId) {
        switch (m_protocolState.load()) {
            case ProtocolState::HANDSHAKING:
                return packetId == 0x00; // Only Handshake allowed
            case ProtocolState::LOGIN:
                // LoginStart packet (avoid pinning specific numeric IDs as they change between versions)  
            case ProtocolState::PLAY:
                return packetId >= 0x80; // Client→Server play packets
        }
        return false;
    }

Consequences

Positive

- Perfect Synchronization: Client and server pipeline changes happen atomically
- No Race Conditions: Single-threaded pipeline modifications eliminate timing issues
- Minecraft Fidelity: Exactly matches Minecraft Java Edition's Netty pipeline pattern
- Extensibility: Easy to add future pipeline stages (encryption, custom compression)
- Error Isolation: Pipeline mismatches detected and handled cleanly

Negative

- I/O Thread Complexity: More logic running on I/O thread vs pure packet forwarding
- Debugging Complexity: Pipeline state changes happen asynchronously from server thread
- State Tracking: Need to carefully manage pipeline state across async operations

Neutral

- Performance: Pipeline checks add minimal overhead per packet
- Memory: Small additional state per connection for pipeline configuration
- Thread Load: I/O thread handles more work but remains non-blocking

Alternatives Considered

Pipeline Changes on Server Thread

    // Server thread processes packet, then changes pipeline
    void LoginPacketListener::onLoginStart(const LoginStartC2SPacket& packet) {
        SendSetCompression(256);
        // Wait for send completion, then enable compression
        m_connection.scheduleCompressionEnable(256);
    }
    Rejected: Race condition between send completion and next packet arrival
    
    Delayed Pipeline Activation
    
    // Enable compression after N packets or timeout
    void enableCompressionAfterDelay() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        enableCompression(256);
    }
    
Rejected: Timing-dependent, unreliable, doesn't match protocol specification

Client Acknowledgment Required

    // Wait for client to confirm compression enabled
    // Client: SetCompressionAck packet
    // Server: Enable compression after receiving ack
    Rejected: Not part of Minecraft protocol, adds roundtrip latency

The I/O thread pipeline flip approach provides the most reliable and protocol-compliant solution while maintaining the architectural benefits of our async networking model.
