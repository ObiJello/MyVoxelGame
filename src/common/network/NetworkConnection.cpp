// File: src/common/network/NetworkConnection.cpp
#include "NetworkConnection.hpp"
#include "PacketTypes.hpp"
#include "../core/Log.hpp"
#include <algorithm>
#include <stdexcept>
#include <zlib.h>

namespace Network {

    std::atomic<uint32_t> NetworkConnection::s_nextConnectionId{1};

    NetworkConnection::NetworkConnection(tcp::socket socket)
        : m_socket(std::move(socket))
        , m_strand(net::make_strand(m_socket.get_executor()))
        , m_connectionId(s_nextConnectionId.fetch_add(1))
        , m_name("Connection#" + std::to_string(m_connectionId))
    {
        // Set TCP_NODELAY to disable Nagle's algorithm
        // This ensures low-latency packet delivery, critical for game networking
        // Especially important on Windows where Nagle's algorithm can cause delays
        try {
            m_socket.set_option(tcp::no_delay(true));
        } catch (const std::exception& e) {
            Log::Warning("[%s] Failed to set TCP_NODELAY: %s", m_name.c_str(), e.what());
        }
        
        m_readBuffer.resize(4096); // Initial read buffer size
        m_stats.connectedTime = std::chrono::steady_clock::now();
    }

    NetworkConnection::~NetworkConnection() {
        if (m_socket.is_open()) {
            Close();
        }
    }

    void NetworkConnection::Start() {
        if (m_state.exchange(ConnectionState::CONNECTED) != ConnectionState::CONNECTED) {
            Log::Info("[%s] Connection started", m_name.c_str());
            OnConnected();
            StartRead();
        }
    }

    void NetworkConnection::Close() {
        auto expected = ConnectionState::CONNECTED;
        if (m_state.compare_exchange_strong(expected, ConnectionState::DISCONNECTING)) {
            Log::Info("[%s] Closing connection gracefully", m_name.c_str());
            
            error_code ec;
            m_socket.shutdown(tcp::socket::shutdown_both, ec);
            m_socket.close(ec);
            
            m_state = ConnectionState::DISCONNECTED;
            OnDisconnected();
        }
    }

    void NetworkConnection::Disconnect() {
        if (m_state.exchange(ConnectionState::DISCONNECTED) != ConnectionState::DISCONNECTED) {
            Log::Info("[%s] Force disconnecting", m_name.c_str());
            
            error_code ec;
            m_socket.close(ec);
            
            OnDisconnected();
        }
    }

    bool NetworkConnection::IsLoopback() const {
        try {
            const auto endpoint = m_socket.remote_endpoint();
            return endpoint.address().is_loopback();
        } catch (...) {
            return false;   // socket already closed — assume not local
        }
    }

    void NetworkConnection::SendPacket(uint8_t packetId, const std::vector<uint8_t>& data,
                                       std::function<void()> onSent) {
        // MC IdDispatchCodec.encode (:47): a packet that is not in the ACTIVE
        // protocol's id map is refused outright rather than written. The
        // encoder is swapped per phase (Connection.setupOutboundProtocol), so
        // in vanilla a play packet during login is not merely unusual — it
        // cannot be encoded at all. Ours logs and drops instead of throwing.
        if (!IsPacketAllowedOutbound(packetId)) {
            Log::Warning("[%s] Refusing to send packet 0x%02X — not part of the "
                         "current protocol phase", m_name.c_str(), packetId);
            return;
        }

        // The frame body: VarInt packet id followed by the payload. This is
        // what the compression stage below operates on, matching MC — the id is
        // inside the compressed region, not outside it.
        std::vector<uint8_t> body;
        body.reserve(5 + data.size());
        EncodeVarInt(static_cast<uint32_t>(packetId), body);
        body.insert(body.end(), data.begin(), data.end());

        // Queue the BODY, not a finished frame. Framing happens at write time
        // in FrameForWire — see the comment there for why that distinction is
        // load-bearing rather than stylistic.
        SendRaw(std::move(body), std::move(onSent));

        m_stats.packetsSent.fetch_add(1);
    }

    std::vector<uint8_t> NetworkConnection::FrameForWire(const std::vector<uint8_t>& body) const {
        std::vector<uint8_t> packet;
        packet.reserve(10 + body.size());

        if (m_compressionThreshold >= 0) {
            // MC CompressionEncoder.encode:
            //
            //     if (uncompressedLength < threshold) { VarInt.write(out, 0);
            //                                           out.writeBytes(uncompressed); }
            //     else { VarInt.write(out, input.length); deflate... }
            //
            // A leading zero means "not compressed"; anything else is the
            // UNCOMPRESSED length, which is how the reader sizes its output.
            std::vector<uint8_t> framed;
            if (body.size() < static_cast<size_t>(m_compressionThreshold)) {
                EncodeVarInt(0, framed);
                framed.insert(framed.end(), body.begin(), body.end());
            } else {
                uLongf bound = compressBound(static_cast<uLong>(body.size()));
                std::vector<uint8_t> deflated(bound);
                if (compress2(deflated.data(), &bound, body.data(),
                              static_cast<uLong>(body.size()), Z_DEFAULT_COMPRESSION) == Z_OK) {
                    deflated.resize(bound);
                    EncodeVarInt(static_cast<uint32_t>(body.size()), framed);
                    framed.insert(framed.end(), deflated.begin(), deflated.end());
                } else {
                    // Deflate failed — send it through uncompressed rather than
                    // dropping the packet. The reader cannot tell the difference.
                    EncodeVarInt(0, framed);
                    framed.insert(framed.end(), body.begin(), body.end());
                }
            }
            EncodeVarInt(static_cast<uint32_t>(framed.size()), packet);
            packet.insert(packet.end(), framed.begin(), framed.end());
        } else {
            EncodeVarInt(static_cast<uint32_t>(body.size()), packet);
            packet.insert(packet.end(), body.begin(), body.end());
        }

        return packet;
    }

    void NetworkConnection::SendPacket(const RawPacket& packet) {
        SendPacket(packet.header.packetId, packet.payload);
    }

    void NetworkConnection::SendRaw(std::vector<uint8_t> data, std::function<void()> onSent) {
        if (m_state != ConnectionState::CONNECTED) {
            Log::Warning("[%s] Attempted to send data on disconnected connection", m_name.c_str());
            // The hook still runs. MC's thenRun fires on the future regardless
            // of outcome (PacketSendListener.java:13), and a caller that uses
            // it to advance protocol state must not be stranded by a dead
            // socket.
            if (onSent) onSent();
            return;
        }

        // Add to send queue (SendScheduler manages outbox limits)
        bool startSend = false;
        const size_t queuedBytes = data.size();
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            m_sendQueue.push_back(PendingSend{std::move(data), std::move(onSent)});
            if (!m_sending) {
                m_sending = true;
                startSend = true;
            }
        }
        
        // Start async send if not already sending
        if (startSend) {
            net::post(m_strand, [self = shared_from_this()]() {
                self->ProcessSendQueue();
            });
        }
        
        m_stats.bytesSent.fetch_add(queuedBytes);
    }

    void NetworkConnection::StartRead() {
        if (m_state != ConnectionState::CONNECTED) {
            return;
        }
        
        // Log::Debug("[%s] Starting async read chain", m_name.c_str());
        
        // Reset read state for new packet
        m_readingHeader = true;
        m_readPos = 0;
        m_currentPacket = RawPacket();
        
        // Start async read for VarInt length (1-5 bytes)
        net::async_read(m_socket,
            net::buffer(m_readBuffer.data(), 1),
            net::bind_executor(m_strand,
                [self = shared_from_this()](const error_code& ec, size_t bytes) {
                    self->HandleReadHeader(ec, bytes);
                }));
    }

    void NetworkConnection::HandleReadHeader(const error_code& error, size_t bytesTransferred) {
        if (error) {
            HandleError(error);
            return;
        }
        
        m_stats.bytesReceived.fetch_add(bytesTransferred);
        
        // Decode VarInt length
        size_t varIntBytes = 0;
        uint32_t packetLength = 0;
        
        // Check if we need more bytes for VarInt
        if ((m_readBuffer[m_readPos] & 0x80) != 0) {
            // Need more bytes, continue reading
            m_readPos++;
            if (m_readPos >= 5) {
                Log::Error("[%s] VarInt too long", m_name.c_str());
                Disconnect();
                return;
            }
            
            net::async_read(m_socket,
                net::buffer(m_readBuffer.data() + m_readPos, 1),
                net::bind_executor(m_strand,
                    [self = shared_from_this()](const error_code& ec, size_t bytes) {
                        self->HandleReadHeader(ec, bytes);
                    }));
            return;
        }
        
        // Complete VarInt received, decode it
        packetLength = DecodeVarInt(m_readBuffer.data(), varIntBytes);
        
        if (packetLength == 0 || packetLength > MAX_PACKET_SIZE) {
            Log::Error("[%s] Invalid packet length: %u", m_name.c_str(), packetLength);
            Disconnect();
            return;
        }
        
        // Resize buffer if needed
        if (m_readBuffer.size() < packetLength) {
            m_readBuffer.resize(packetLength);
        }
        
        // Read packet ID and payload
        m_currentPacket.header.length = packetLength;
        m_readingHeader = false;
        
        net::async_read(m_socket,
            net::buffer(m_readBuffer.data(), packetLength),
            net::bind_executor(m_strand,
                [self = shared_from_this()](const error_code& ec, size_t bytes) {
                    self->HandleReadPayload(ec, bytes);
                }));
    }

    void NetworkConnection::HandleReadPayload(const error_code& error, size_t bytesTransferred) {
        if (error) {
            HandleError(error);
            return;
        }
        
        m_stats.bytesReceived.fetch_add(bytesTransferred);
        
        // Log raw bytes for debugging
        std::string hexDump;
        for (size_t i = 0; i < std::min(size_t(10), bytesTransferred); i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", m_readBuffer[i]);
            hexDump += buf;
        }
        
        // Undo the compression stage first, so everything below sees the same
        // "VarInt id + payload" body it always did.
        //
        // MC CompressionDecoder: a leading VarInt of 0 means the rest is raw;
        // otherwise it is the uncompressed length to inflate to.
        std::vector<uint8_t> inflated;
        const uint8_t* body = m_readBuffer.data();
        size_t bodySize = bytesTransferred;

        if (m_compressionThreshold >= 0) {
            size_t lenBytes = 0;
            uint32_t uncompressedLength = 0;
            try {
                uncompressedLength = DecodeVarInt(m_readBuffer.data(), lenBytes);
            } catch (const std::exception& e) {
                Log::Error("[%s] Bad compressed frame header: %s", m_name.c_str(), e.what());
                Disconnect();
                return;
            }
            if (uncompressedLength == 0) {
                body     = m_readBuffer.data() + lenBytes;
                bodySize = bytesTransferred - lenBytes;
            } else {
                if (uncompressedLength > MAX_PACKET_SIZE) {
                    Log::Error("[%s] Compressed frame claims %u bytes", m_name.c_str(),
                               uncompressedLength);
                    Disconnect();
                    return;
                }
                inflated.resize(uncompressedLength);
                uLongf out = uncompressedLength;
                const int rc = uncompress(inflated.data(), &out,
                                          m_readBuffer.data() + lenBytes,
                                          static_cast<uLong>(bytesTransferred - lenBytes));
                if (rc != Z_OK || out != uncompressedLength) {
                    Log::Error("[%s] Inflate failed (rc=%d, got %lu of %u)",
                               m_name.c_str(), rc, static_cast<unsigned long>(out),
                               uncompressedLength);
                    Disconnect();
                    return;
                }
                body     = inflated.data();
                bodySize = uncompressedLength;
            }
        }

        // Extract packet ID as VarInt
        size_t packetIdBytes = 0;
        uint32_t packetId = 0;
        try {
            packetId = DecodeVarInt(body, packetIdBytes);
        } catch (const std::exception& e) {
            Log::Error("[%s] Failed to decode packet ID: %s, raw bytes: %s", 
                      m_name.c_str(), e.what(), hexDump.c_str());
            Disconnect();
            return;
        }
        
        if (packetId > 255) {
            Log::Error("[%s] Invalid packet ID: 0x%X, raw bytes: %s", 
                      m_name.c_str(), packetId, hexDump.c_str());
            Disconnect();
            return;
        }
        
        m_currentPacket.header.packetId = static_cast<uint8_t>(packetId);
        
        // Extract payload (remaining bytes after VarInt packet ID)
        m_currentPacket.payload.assign(body + packetIdBytes, body + bodySize);
        
        // Debug log for received packets - only log in base class if not a known packet type
        // Client and Server connections will log their own specific packets  
        if (m_currentPacket.header.packetId != 0x81 && 
            m_currentPacket.header.packetId != static_cast<uint8_t>(PacketId::PlayerMoveC2S) &&
            m_currentPacket.header.packetId != static_cast<uint8_t>(PacketId::KeepAliveS2C)) {
            Log::Debug("[%s] Received packet ID 0x%02X, size: %zu bytes", 
                      m_name.c_str(), m_currentPacket.header.packetId, 
                      m_currentPacket.payload.size());
        }
        
        m_stats.packetsReceived.fetch_add(1);
        
        // Decode packet on I/O thread (creates typed packet)
        try {
            PacketPtr packet = DecodePacket(m_currentPacket.header.packetId, m_currentPacket.payload);
            if (packet) {
                // Debug logging for critical packets on Windows debugging
                if (m_currentPacket.header.packetId == static_cast<uint8_t>(Network::PacketId::LoginStart) ||
                    m_currentPacket.header.packetId == static_cast<uint8_t>(Network::PacketId::KeepAliveC2S)) {
                    Log::Debug("[%s] I/O thread queueing packet ID 0x%02X for main thread", 
                              m_name.c_str(), m_currentPacket.header.packetId);
                }
                
                // Queue for main thread processing
                IncomingPacket incoming(std::move(packet));
                if (!m_incomingPackets.try_push(std::move(incoming))) {
                    Log::Warning("[%s] Incoming packet queue full, dropping packet ID 0x%02X", 
                                m_name.c_str(), m_currentPacket.header.packetId);
                } else {
                    // Log successful queueing for debugging
                    if (m_currentPacket.header.packetId == static_cast<uint8_t>(Network::PacketId::LoginStart)) {
                        Log::Debug("[%s] LoginStart packet successfully queued (queue size: %zu)", 
                                  m_name.c_str(), m_incomingPackets.Size());
                    }
                }
            } else if (ShouldDeferPacket(m_currentPacket.header.packetId)) {
                // Not decoded into a typed packet, but it still must not run
                // here — this is the network I/O thread. Queue it so the legacy
                // registry handler runs on the owning thread, which is MC's
                // PacketUtils.ensureRunningOnSameThread ->
                // PacketProcessor.scheduleIfPossible path.
                IncomingPacket incoming(std::make_unique<RawPayloadPacket>(
                    m_currentPacket.header.packetId, m_currentPacket.payload));
                if (!m_incomingPackets.try_push(std::move(incoming))) {
                    Log::Warning("[%s] Incoming packet queue full, dropping packet ID 0x%02X",
                                m_name.c_str(), m_currentPacket.header.packetId);
                }
            } else {
                // Handled inline, on purpose. MC's login/handshake listeners
                // carry no ensureRunningOnSameThread call and run on the Netty
                // thread; so do ours. See ShouldDeferPacket for which packets
                // this covers and why compression MUST be one of them.
                OnPacketReceived(m_currentPacket.header.packetId, m_currentPacket.payload);
            }
        } catch (const std::exception& e) {
            // Malformed packet — almost always an internet scanner or bot probing the
            // open port (never happens for legitimate clients). Disconnect immediately
            // instead of looping back into StartRead. If we keep reading, the strand
            // callbacks hold shared_from_this(), so the half-dead connection stays
            // alive in memory; over hours of scanner traffic those accumulate until
            // file descriptors / heap exhaust and the process crashes.
            Log::Error("[%s] Exception decoding packet: %s — disconnecting", m_name.c_str(), e.what());
            Disconnect();
            return;
        }

        // Start reading next packet
        StartRead();
    }

    void NetworkConnection::ProcessSendQueue() {
        // Use shared_ptr to keep data alive during async operation
        auto entry = std::make_shared<PendingSend>();
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (m_sendQueue.empty()) {
                m_sending = false;
                return;
            }
            *entry = std::move(m_sendQueue.front());
            m_sendQueue.pop_front();
        }

        // FRAME HERE, on the strand, immediately before the write — not when
        // the packet was enqueued.
        //
        // This is Netty's CompressionEncoder position: a pipeline stage that
        // runs at write time, so "everything written after the encoder was
        // installed is compressed, everything before it is not" is true by
        // construction. Framing at enqueue time instead makes that depend on a
        // race between the caller and this strand — enable compression in a
        // send-completion hook and the very next packet may already have been
        // framed under the old rules, which the peer would then mis-parse.
        entry->data = FrameForWire(entry->data);

        // Async write - data is kept alive by the shared_ptr captured in lambda
        net::async_write(m_socket,
            net::buffer(entry->data),
            net::bind_executor(m_strand,
                [self = shared_from_this(), entry](const error_code& ec, size_t bytes) {
                    // entry shared_ptr keeps the buffer alive until this handler completes.
                    //
                    // The hook runs BEFORE HandleWrite queues the next frame,
                    // and before the error path, so a hook that reframes the
                    // stream (compression) takes effect for everything after
                    // this packet and nothing before it. Unconditional, as in
                    // PacketSendListener.thenRun.
                    if (entry->onSent) entry->onSent();
                    self->HandleWrite(ec, bytes);
                }));
    }

    void NetworkConnection::HandleWrite(const error_code& error, size_t bytesTransferred) {
        if (error) {
            HandleError(error);
            return;
        }
        
        // Log::Debug("[%s] Successfully wrote %zu bytes to socket", m_name.c_str(), bytesTransferred);
        
        // Continue processing send queue
        ProcessSendQueue();
    }

    void NetworkConnection::HandleError(const error_code& error) {
        if (error == net::error::eof || 
            error == net::error::connection_reset ||
            error == net::error::broken_pipe) {
            Log::Info("[%s] Connection closed by peer", m_name.c_str());
        } else if (error != net::error::operation_aborted) {
            Log::Error("[%s] Network error: %s", m_name.c_str(), error.message().c_str());
        }
        
        OnError(error);
        Disconnect();
    }

    net::ip::tcp::endpoint NetworkConnection::GetRemoteEndpoint() const {
        error_code ec;
        return m_socket.remote_endpoint(ec);
    }

    net::ip::tcp::endpoint NetworkConnection::GetLocalEndpoint() const {
        error_code ec;
        return m_socket.local_endpoint(ec);
    }

    void NetworkConnection::EncodeVarInt(uint32_t value, std::vector<uint8_t>& buffer) {
        while ((value & 0xFFFFFF80) != 0) {
            buffer.push_back((value & 0x7F) | 0x80);
            value >>= 7;
        }
        buffer.push_back(value & 0x7F);
    }

    uint32_t NetworkConnection::DecodeVarInt(const uint8_t* data, size_t& bytesRead) {
        uint32_t value = 0;
        size_t position = 0;
        uint8_t currentByte;
        
        bytesRead = 0;
        do {
            if (bytesRead >= 5) {
                throw std::runtime_error("VarInt too big");
            }
            
            currentByte = data[bytesRead];
            value |= (currentByte & 0x7F) << position;
            
            bytesRead++;
            position += 7;
        } while ((currentByte & 0x80) != 0);
        
        return value;
    }

    size_t NetworkConnection::GetVarIntSize(uint32_t value) {
        size_t size = 0;
        while ((value & 0xFFFFFF80) != 0) {
            size++;
            value >>= 7;
        }
        return size + 1;
    }

} // namespace Network