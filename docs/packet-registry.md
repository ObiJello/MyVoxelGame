# Packet Registry

## Packet ID Allocation

Note: Minecraft uses VarInt packet IDs that are state-scoped (HANDSHAKING/LOGIN/PLAY/etc.) and change between protocol versions. The allocation shown below is our engine's local convention for simplified implementation.

### Handshake & Setup Packets (0x00-0x0F)

| ID | Direction | State | Name | Listener | Payload |
|----|-----------|-------|------|----------|---------|
| 0x00 | C2S | HANDSHAKING | Handshake | `HandshakePacketListener::onHandshake` | VarInt(protocol), String(address), UShort(port), VarInt(nextState) |
| 0x01 | C2S | STATUS | StatusRequest | `StatusPacketListener::onStatusRequest` | Empty |
| 0x02 | S2C | STATUS | StatusResponse | Client | String(jsonResponse) |
| 0x03 | C2S | STATUS | Ping | `StatusPacketListener::onPing` | Long(payload) |
| 0x04 | S2C | STATUS | Pong | Client | Long(payload) |
| 0x05 | C2S | LOGIN | LoginStart | `LoginPacketListener::onLoginStart` | String(name) |
| 0x06 | S2C | LOGIN | LoginSuccess | Client | String(uuid), String(name) |
| 0x07 | S2C | ALL | Disconnect | Client | String(reason) |
| 0x08 | S2C | LOGIN | SetCompression | Client | VarInt(threshold) |

Note: Above IDs are our engine's fixed assignment. In true Minecraft protocol, these vary by version and are VarInt-encoded.
| 0x09-0x0F | - | - | Reserved | - | TBD |

### Server → Client Packets (0x10-0x7F)

| ID | Direction | State | Name | Listener | Payload |
|----|-----------|-------|------|----------|---------|
| 0x10 | S2C | PLAY | ChunkDataS2C | Client | ChunkPos(x,z), SerializedChunkData |
| 0x11 | S2C | PLAY | UnloadChunkS2C | Client | ChunkPos(x,z) |
| 0x12 | S2C | PLAY | BlockChangeS2C | Client | BlockPos(x,y,z), VarInt(blockState) |
| 0x13 | S2C | PLAY | MultiBlockChangeS2C | Client | ChunkPos(x,z), Array[BlockChange] |
| 0x14 | S2C | PLAY | PlayerUpdateS2C | Client | Vec3(position), Vec2(rotation) |
| 0x15 | S2C | PLAY | EntitySpawn | Client | TBD |
| 0x16 | S2C | PLAY | EntityMove | Client | TBD |
| 0x17 | S2C | PLAY | EntityDestroy | Client | TBD |
| 0x18 | S2C | PLAY | ChatMessageS2C | Client | String(message), Byte(position) |
| 0x19 | S2C | PLAY | TimeUpdate | Client | Long(worldAge), Long(timeOfDay) |
| 0x1A | S2C | PLAY | WeatherChange | Client | TBD |
| 0x1B | S2C | PLAY | KeepAliveS2C | Client | Long(keepAliveId) |
| 0x1C | S2C | PLAY | PlayerListUpdate | Client | TBD |
| 0x1D | S2C | PLAY | PlayerAbilities | Client | Byte(flags), Float(flySpeed), Float(walkSpeed) |
| 0x1E | S2C | PLAY | WorldSpawn | Client | BlockPos(x,y,z) |
| 0x1F | S2C | PLAY | Reserved | Client | TBD |
| 0x20-0x21 | - | - | (Duplicate entries removed) | - | - |
| 0x22-0x7F | S2C | PLAY | Reserved | Client | TBD |

### Client → Server Packets (0x80-0xFF)

| ID | Direction | State | Name | Listener | Payload |
|----|-----------|-------|------|----------|---------|
| 0x80 | C2S | PLAY | BlockActionC2S | `IServerPlayPacketListener::onBlockAction` | BlockPos(x,y,z), VarInt(action), VarInt(blockId) |
| 0x81 | C2S | PLAY | PlayerMoveC2S | `IServerPlayPacketListener::onPlayerMove` | Vec3(position), Vec2(rotation), Long(timestamp) |
| 0x82 | C2S | PLAY | ChatMessageC2S | `IServerPlayPacketListener::onChatMessage` | String(message), Boolean(isCommand) |
| 0x83 | C2S | PLAY | ClientConfigC2S | `IServerPlayPacketListener::onClientConfig` | TBD |
| 0x84 | C2S | PLAY | UseItem | `IServerPlayPacketListener::onUseItem` | TBD |
| 0x85 | C2S | PLAY | PlayerAction | `IServerPlayPacketListener::onPlayerAction` | TBD |
| 0x86 | C2S | PLAY | KeepAliveC2S | `IServerPlayPacketListener::onKeepAlive` | Long(keepAliveId) |
| 0x87 | C2S | PLAY | PlayerRotation | `IServerPlayPacketListener::onPlayerRotation` | TBD |
| 0x88 | C2S | PLAY | PlayerPosition | `IServerPlayPacketListener::onPlayerPosition` | TBD |
| 0x89 | C2S | PLAY | PlayerPosRot | `IServerPlayPacketListener::onPlayerPosRot` | TBD |
| 0x8A-0xFF | C2S | PLAY | Reserved | `IServerPlayPacketListener` | TBD |

## Protocol States and Packet Listeners

### State Transitions
```
HANDSHAKING → STATUS (nextState=1) or LOGIN (nextState=2)
LOGIN → PLAY (after LoginSuccess)
```

### Packet Listener Interface

Each protocol state has a dedicated packet listener interface:

```cpp
// src/server/network/listeners/IHandshakePacketListener.hpp
class IHandshakePacketListener {
    virtual void onHandshake(const HandshakeC2SPacket& packet) = 0;
    virtual void onDisconnect(const std::string& reason) = 0;
};

// src/server/network/listeners/ILoginPacketListener.hpp  
class ILoginPacketListener {
    virtual void onLoginStart(const LoginStartC2SPacket& packet) = 0;
    virtual void onDisconnect(const std::string& reason) = 0;
};

// src/server/network/listeners/IServerPlayPacketListener.hpp
class IServerPlayPacketListener {
    virtual void onBlockAction(const BlockActionC2SPacket& packet) = 0;
    virtual void onPlayerMove(const PlayerMoveC2SPacket& packet) = 0;
    virtual void onChatMessage(const ChatMessageC2SPacket& packet) = 0;
    virtual void onKeepAlive(const KeepAliveC2SPacket& packet) = 0;
    // ... more play packets
};
```

### Listener Swapping
Protocol state changes happen on the I/O thread via `ServerConnection::setProtocolState()`:

```cpp
void ServerConnection::setProtocolState(ProtocolState state) {
    switch (state) {
        case ProtocolState::HANDSHAKING:
            m_listener = std::make_unique<HandshakePacketListener>(*this);
            break;
        case ProtocolState::LOGIN:
            m_listener = std::make_unique<LoginPacketListener>(*this);
            break;
        case ProtocolState::PLAY:
            m_listener = std::make_unique<PlayPacketListener>(*this);
            break;
    }
}
```

## VarInt/VarLong Encoding Rules

### VarInt (32-bit)
- **Range**: -2,147,483,648 to 2,147,483,647
- **Encoding**: 7 bits per byte, MSB indicates continuation
- **Max Size**: 5 bytes
- **Read Cap**: Throw exception if more than 5 bytes read

### VarLong (64-bit)
- **Range**: Full 64-bit signed integer range
- **Encoding**: 7 bits per byte, MSB indicates continuation
- **Max Size**: 10 bytes
- **Read Cap**: Throw exception if more than 10 bytes read

### Implementation
See `src/common/network/PacketRegistry.hpp` lines 129-216 for full VarInt/VarLong encoding/decoding logic.

## Packet Size Limits

### Per-Packet Limits
- **Maximum packet size**: 2MB (prevents memory exhaustion)
- **String fields**: 32KB max length each
- **Array fields**: 65536 elements max each

### Connection Limits
- **Packet rate**: 1000 packets/second max per connection
- **Byte rate**: 1MB/second max per connection
- **Keep-alive timeout**: 30 seconds

### VarInt Security
- **Read timeout**: 100ms max per VarInt decode
- **Malformed detection**: Invalid continuation bits cause disconnect
- **Length validation**: Oversized VarInt causes immediate disconnect

## Future Packet Expansion

Reserved packet ID ranges for future features:
- **0x09-0x0F**: Additional handshake/login packets
- **0x22-0x7F**: Additional server→client packets
- **0x8A-0xFF**: Additional client→server packets

All future packets must:
1. Add entry to this registry table
2. Implement corresponding listener method
3. Add to `PacketIdToString()` function
4. Document payload format and validation rules