ADR-0002: Typed Packets and Protocol Listeners

Status

Accepted

Context

The networking system needs to handle dozens of different packet types across multiple protocol states (HANDSHAKING, LOGIN, PLAY). Each packet type has different payload formats and requires
different processing logic.

We need to solve several problems:
- Type Safety: Prevent runtime errors from incorrect packet handling
- Protocol Evolution: Easy addition of new packet types as the protocol evolves
- State Management: Different packet sets valid in different connection phases
- Performance: Efficient packet dispatch without virtual function overhead
- Thread Safety: Decode on I/O thread, process on main thread safely

We evaluated several architectural approaches:
1. Giant switch statement: Fast but unmaintainable, breaks OCP
2. Virtual packet base class: Flexible but requires virtual dispatch overhead
3. Function pointer table: Fast but limited type safety
4. Visitor pattern with listeners: Type-safe, extensible, mirrors Minecraft architecture

Decision

We will use typed packet structs with protocol-specific listeners, implementing a visitor-like pattern where:
- Packets are decoded on I/O thread into strongly-typed structs
- Each protocol state has a dedicated packet listener interface
- Listeners are swapped during protocol state transitions
- Main thread processes packets by calling listener methods

Rationale

Minecraft Java Edition Pattern

This directly mirrors Minecraft's client-server networking architecture:
- ServerLoginNetworkHandler, ServerPlayNetworkHandler in Minecraft Java
- Each handler has strongly-typed methods like onLoginStart(LoginStartC2SPacket packet)
- Protocol state transitions swap the active network handler
- Netty pipeline decodes raw bytes → typed packet objects → handler methods

Type Safety Benefits

    // Compile-time type checking
    void ILoginPacketListener::onLoginStart(const LoginStartC2SPacket& packet) {
        // packet.name is guaranteed to be a valid string
        // No casting or runtime type checking needed
    }
    
    // vs error-prone alternative:
    void handlePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
        switch (packetId) {
            case 0x05: // LoginStart - what if we get the wrong packet?
            // Manual parsing, potential for buffer overruns
            break;
        }
    }

Protocol State Isolation

    // Each state has its own listener interface
    class IHandshakePacketListener {
        virtual void onHandshake(const HandshakeC2SPacket& packet) = 0;
        virtual void onDisconnect(const std::string& reason) = 0;
    };
    
    class ILoginPacketListener {
        virtual void onLoginStart(const LoginStartC2SPacket& packet) = 0;
        virtual void onDisconnect(const std::string& reason) = 0;
    };
    
    class IServerPlayPacketListener {
        virtual void onBlockAction(const BlockActionC2SPacket& packet) = 0;
        virtual void onPlayerMove(const PlayerMoveC2SPacket& packet) = 0;
        virtual void onChatMessage(const ChatMessageC2SPacket& packet) = 0;
        // ... 20+ more play packets
    };

Implementation Architecture

Packet Decoding (I/O Thread)

    // In ServerConnection::OnPacketReceived()
    Network::PacketPtr ServerConnection::DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);

        switch (m_phase) {
            case ConnectionPhase::HANDSHAKING:
                return DecodeHandshakePacket(packetId, reader);
            case ConnectionPhase::LOGIN:
                return DecodeLoginPacket(packetId, reader);
            case ConnectionPhase::PLAY:
                return DecodePlayPacket(packetId, reader);
        }

        return nullptr; // Unknown packet
    }
    
    std::unique_ptr<HandshakeC2SPacket> DecodeHandshakePacket(uint8_t id, PacketReader& reader) {
        if (id == 0x00) {
            auto packet = std::make_unique<HandshakeC2SPacket>();
            packet->protocolVersion = reader.ReadVarInt();
            packet->serverAddress = reader.ReadString();
            packet->serverPort = reader.ReadShort();
            packet->nextState = reader.ReadVarInt();
            return packet;
        }
        return nullptr;
    }

Packet Processing (Main Thread)

    // In ServerConnection::tick() - called by server thread
    void ServerConnection::tick() {
        auto packets = m_inboundPackets.DrainAll();

        for (const auto& packet : packets) {
            try {
                // Dispatch to current listener (visitor pattern)
                packet->apply(*m_listener);
            } catch (const std::exception& e) {
                Log::Warning("Packet processing failed: %s", e.what());
            }
        }
    }
    
    // Packet implements visitor interface
    void HandshakeC2SPacket::apply(IPacketListener& listener) {
        // Downcast to specific listener type  
        auto* handshakeListener = dynamic_cast<IHandshakePacketListener*>(&listener);
        if (handshakeListener) {
            handshakeListener->onHandshake(*this);
        }
    }

Listener Swapping (I/O Thread)

    // Protocol state transitions happen immediately on I/O thread
    void ServerConnection::setProtocolState(Network::ProtocolState state) {
        switch (state) {
            case ProtocolState::HANDSHAKING:
                m_listener = std::make_unique<HandshakePacketListener>(*this);
                break;
            case ProtocolState::LOGIN:
                // Registry flip: Different packet IDs now valid
                m_listener = std::make_unique<LoginPacketListener>(*this);
                break;
            case ProtocolState::PLAY:
                m_listener = std::make_unique<PlayPacketListener>(*this);
                break;
        }
    }

Consequences

Positive

- Type Safety: Compile-time guarantees for packet field access
- Maintainability: Adding new packets only requires updating one listener interface
- Performance: No vtable lookup during packet dispatch (templates + inlining)
- Debugging: Stack traces show exact packet type and handler method
- Protocol Fidelity: Direct mapping to Minecraft's proven architecture

Negative

- Code Generation: Each packet needs decode/encode methods (could be automated)
- Binary Size: Template instantiation creates more code than switch statements
- Complexity: More moving parts than simple switch-based dispatch

Thread Safety Guarantees

- I/O Thread: Decodes raw bytes → typed packets → enqueues for main thread
- Main Thread: Drains packet queue → calls strongly-typed listener methods
- No Shared State: Packets are value types, listeners are connection-local
- Listener Swapping: Only I/O thread modifies current listener (no races)

Extension Pattern

Adding New Packets

1. Define packet struct with payload fields:


    struct NewFeatureC2SPacket {
        uint32_t featureId;
        std::string data;
    
        void apply(IPacketListener& listener) override;
    };

2. Add to packet registry enum:


    enum class PacketId : uint8_t {
        // ... existing packets
        NewFeatureC2S = 0x90,
    };

3. Add listener method:


    class IServerPlayPacketListener {
        // ... existing methods
       virtual void onNewFeature(const NewFeatureC2SPacket& packet) = 0;
    };

4. Implement in concrete listener:


    class PlayPacketListener : public IServerPlayPacketListener {
        void onNewFeature(const NewFeatureC2SPacket& packet) override {
            // Handle new feature logic
        }
    };

Protocol Version Support

Future protocol versions can reuse the same listener interfaces:

    // v760 and v761 both use same play listener
    if (protocolVersion == 760 || protocolVersion == 761) {
        return DecodePlayPacket_v760(packetId, reader);
    }

Alternatives Considered

Giant Switch Statement

    void HandlePacket(uint8_t id, const std::vector<uint8_t>& payload) {
        switch (id) {
            case 0x00: /* handshake logic */ break;
            case 0x05: /* login start logic */ break;
            // ... 50+ cases
        }
    }
Rejected: Violates Open/Closed Principle, becomes unmaintainable

Virtual Packet Base Class

    class IPacket {
        virtual void process(ServerConnection& conn) = 0;
    };
Rejected: Virtual function overhead, harder to optimize, less type-safe

Function Pointer Table

    using PacketHandler = std::function<void(const std::vector<uint8_t>&)>;
    std::unordered_map<uint8_t, PacketHandler> handlers;
Rejected: Still requires manual payload parsing, no compile-time type checking

The typed packets + listener pattern provides the best balance of type safety, performance, and architectural clarity while directly mirroring Minecraft's proven networking design.