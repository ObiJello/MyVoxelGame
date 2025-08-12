Packet Framing and Wire Format

Uncompressed Frame Format

Standard packet framing before compression is enabled:

[VarInt: Packet Length] [VarInt: Packet ID] [Payload Data]

Example: LoginStart Packet

Packet ID: LoginStart (value depends on protocol version)
Player Name: "Steve" (5 bytes)

Wire format:
[VarInt: 6]        // Packet Length = 6 bytes (1 byte ID + 5 bytes payload)
[VarInt: LoginStart ID]  // Packet ID (value varies by protocol version)
[VarInt: 5]        // String length
[UTF-8: "Steve"]   // Player name

Compressed Frame Format

After SetCompression packet (0x08) is sent, all subsequent packets use compression:

[VarInt: Packet Length] [VarInt: Data Length] [Compressed or Uncompressed Data]

Compression Rules

- Data Length = 0: Packet is below compression threshold, data is uncompressed
- Data Length > 0: Data Length indicates uncompressed size, data is zlib-compressed

Example: Compressed ChunkDataS2C

Uncompressed: [ChunkDataS2C ID] [chunk data...] = 8192 bytes
Compressed with zlib: 2048 bytes

Wire format:
[VarInt: TotalLength]  // Total packet length (compressed data + data length VarInt)
[VarInt: 8192]         // Data Length = 8192 (uncompressed size)
[Compressed: 2048 bytes] // zlib compressed payload

Example: Uncompressed Small Packet

Packet smaller than compression threshold (256 bytes):

Wire format:
[VarInt: 10]       // Packet Length = 10 bytes
[VarInt: 0]        // Data Length = 0 (uncompressed)
[VarInt: KeepAliveC2S ID] // Packet ID
[Long: keepalive]  // 8-byte keepalive payload

VarInt/VarLong Wire Encoding

VarInt Encoding Algorithm

    void VarInt::Encode(uint32_t value, std::vector<uint8_t>& buffer) {
        while ((value & 0xFFFFFF80) != 0) {
            buffer.push_back((value & 0x7F) | 0x80);  // Set continuation bit
            value >>= 7;
        }
        buffer.push_back(value & 0x7F);  // Final byte, no continuation
    }

VarInt Examples

- Value 0: 0x00
- Value 127: 0x7F
- Value 128: 0x80 0x01
- Value 16383: 0xFF 0x7F
- Value 16384: 0x80 0x80 0x01

VarLong Encoding

Same algorithm as VarInt but for 64-bit values, max 10 bytes.

Pipeline State Changes

Compression Activation

The compression pipeline flip happens on the I/O thread immediately after sending SetCompression:

    // In LoginPacketListener::onLoginStart()
    void LoginPacketListener::onLoginStart(const LoginStartC2SPacket& packet) {
        // Send compression setup
        m_connection.SendSetCompression(256);  // Threshold = 256 bytes

        // Pipeline flip happens here on I/O thread
        m_connection.enableCompression(256);

        // All subsequent packets will be compressed
        m_connection.SendLoginSuccess(playerId, playerName);
    }

Note: Actual packet IDs for LoginStart, SetCompression, LoginSuccess vary by Minecraft protocol version.

Encryption Setup (Future)

Encryption pipeline flip would follow similar pattern:

    // After encryption response packet
    m_connection.enableEncryption(sharedSecret);  // AES/CFB8 mode
    // All subsequent packets encrypted

Packet Size Validation

Reading Safety

VarInt/VarLong readers include bounds checking to prevent abuse:

    uint32_t VarInt::Decode(const uint8_t* data, size_t& bytesRead) {
        // ... decoding logic ...
        if (bytesRead >= 5) {
            throw std::runtime_error("VarInt too big");  // Prevent infinite reads
        }
        // ... continue reading ...
    }

Packet Length Limits

- Maximum packet length: 2MB per packet
- VarInt read limit: Maximum 5 bytes (prevents CPU abuse)
- VarLong read limit: Maximum 10 bytes
- String length limit: 32KB per string field

Connection Protection

    // In ServerConnection packet processing
    if (packetLength > MAX_PACKET_SIZE) {
        SendDisconnect("Packet too large");
        Close();
        return;
    }
    
    if (stringLength > MAX_STRING_LENGTH) {
        SendDisconnect("String too long");
        Close();
        return;
    }

Compression Implementation Details

Zlib Integration

    #include <zlib.h>
    
    std::vector<uint8_t> CompressData(const std::vector<uint8_t>& data) {
        uLongf compressedSize = compressBound(data.size());
        std::vector<uint8_t> compressed(compressedSize);

        int result = compress(compressed.data(), &compressedSize,
                           data.data(), data.size());
        if (result != Z_OK) {
            throw std::runtime_error("Compression failed");
        }

        compressed.resize(compressedSize);
        return compressed;
    }

Compression Threshold

- Default threshold: 256 bytes
- Below threshold: Send uncompressed (Data Length = 0)
- Above threshold: Compress with zlib, include original size
- Benefit calculation: Only compress if result is smaller

Decompression

    std::vector<uint8_t> DecompressData(const std::vector<uint8_t>& compressed,
    size_t originalSize) {
        std::vector<uint8_t> decompressed(originalSize);
        uLongf decompressedSize = originalSize;

        int result = uncompress(decompressed.data(), &decompressedSize,
                             compressed.data(), compressed.size());
        if (result != Z_OK) {
            throw std::runtime_error("Decompression failed");
        }

        return decompressed;
    }

Frame Processing Order

Outbound (Server → Client)

1. Serialize packet to bytes with packet ID
2. Compress if above threshold, set Data Length
3. Frame with Packet Length VarInt header
4. Send via async_write on connection strand

Inbound (Client → Server)

1. Read Packet Length VarInt from socket
2. Read exactly Packet Length bytes
3. Decompress if Data Length > 0
4. Parse Packet ID and payload
5. Enqueue parsed packet for main thread processing

This framing ensures reliable packet boundaries over TCP and supports efficient compression for large data packets like chunk streaming.