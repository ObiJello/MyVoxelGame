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

    // Transport question ONLY: "did this socket come from this machine".
    //
    // MUST NOT be used as MC's isSingleplayerOwner. A friend joining through
    // the relay makes the host dial OUT to the friends service and hands that
    // socket to NetworkServer::AdoptConnection, and CLAUDE.md documents the
    // hosting machine as pointing friends_service at 127.0.0.1 for NAT
    // hairpin — so a remote WAN player genuinely presents a loopback remote
    // endpoint. Exempting on this basis would hand a remote player the host's
    // keep-alive and read-timeout exemption.
    //
    // The owner test is a NAME match against the singleplayer profile, exactly
    // as in MC (IntegratedServer.java:269). See
    // IntegratedServer::OnPlayerJoined and ServerConnection::IsSingleplayerOwner.
    //
    // Legitimate use, and the reason it exists: skipping compression for a
    // same-machine connection, MC's isMemoryConnection (LoginPacketListener).
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

        // Bulk reads: pull whatever the socket has into the buffer and parse
        // every complete frame out of it. This replaced a chain that issued
        // two async_reads PER PACKET (one byte for the length VarInt, then the
        // payload), each a syscall and a strand hop. With a hundred thousand
        // tracked entities the stream is hundreds of thousands of 20-byte
        // frames a second, and the per-frame syscall pair — not the CPU on
        // either side — was what left the client twenty seconds behind the
        // server. Netty reads into a ByteBuf and lets the frame decoder split
        // it; this is the same shape.
        if (m_readBuffer.size() < kReadChunk) m_readBuffer.resize(kReadChunk);
        if (m_readFill == m_readBuffer.size()) {
            // A frame larger than the whole buffer is still arriving; grow.
            m_readBuffer.resize(m_readBuffer.size() * 2);
        }

        m_socket.async_read_some(
            net::buffer(m_readBuffer.data() + m_readFill, m_readBuffer.size() - m_readFill),
            net::bind_executor(m_strand,
                [self = shared_from_this()](const error_code& ec, size_t bytes) {
                    self->HandleReadSome(ec, bytes);
                }));
    }

    void NetworkConnection::HandleReadSome(const error_code& error, size_t bytesTransferred) {
        if (error) {
            HandleError(error);
            return;
        }

        m_stats.bytesReceived.fetch_add(bytesTransferred);
        m_readFill += bytesTransferred;

        size_t pos = 0;
        while (pos < m_readFill) {
            // Length VarInt, decoded against what has actually arrived — a
            // partial VarInt at the end of the buffer must wait for more
            // bytes, not read past the fill mark into stale data.
            uint32_t frameLength = 0;
            size_t   lenBytes    = 0;
            bool     complete    = false;
            for (size_t i = pos; i < m_readFill && i < pos + 5; ++i) {
                const uint8_t b = m_readBuffer[i];
                frameLength |= static_cast<uint32_t>(b & 0x7F) << (7 * (i - pos));
                ++lenBytes;
                if ((b & 0x80) == 0) { complete = true; break; }
            }
            if (!complete) {
                if (lenBytes >= 5) {
                    Log::Error("[%s] VarInt too long", m_name.c_str());
                    Disconnect();
                    return;
                }
                break;   // need more bytes for the length itself
            }
            if (frameLength == 0 || frameLength > MAX_PACKET_SIZE) {
                Log::Error("[%s] Invalid packet length: %u", m_name.c_str(), frameLength);
                Disconnect();
                return;
            }
            const size_t frameEnd = pos + lenBytes + frameLength;
            if (frameEnd > m_readFill) {
                // Incomplete frame. Make sure the buffer can hold all of it
                // once the unconsumed prefix is compacted to the front.
                const size_t needed = lenBytes + frameLength;
                if (m_readBuffer.size() - pos < needed) {
                    // Compact first so the resize below is sized from zero.
                    std::memmove(m_readBuffer.data(), m_readBuffer.data() + pos, m_readFill - pos);
                    m_readFill -= pos;
                    pos = 0;
                    if (m_readBuffer.size() < needed) m_readBuffer.resize(needed);
                }
                break;
            }

            if (!ProcessFrame(m_readBuffer.data() + pos + lenBytes, frameLength)) {
                return;   // disconnected inside the frame
            }
            pos = frameEnd;

            // A frame handled inline may have closed the connection (a kick
            // during login); stop parsing rather than feeding a dead peer.
            if (m_state != ConnectionState::CONNECTED) return;
        }

        if (pos > 0) {
            std::memmove(m_readBuffer.data(), m_readBuffer.data() + pos, m_readFill - pos);
            m_readFill -= pos;
        }

        StartRead();
    }

    bool NetworkConnection::ProcessFrame(const uint8_t* frame, size_t frameSize) {
        // Undo the compression stage first, so everything below sees the same
        // "VarInt id + payload" body it always did.
        //
        // MC CompressionDecoder: a leading VarInt of 0 means the rest is raw;
        // otherwise it is the uncompressed length to inflate to.
        std::vector<uint8_t> inflated;
        const uint8_t* body = frame;
        size_t bodySize = frameSize;

        if (m_compressionThreshold >= 0) {
            size_t lenBytes = 0;
            uint32_t uncompressedLength = 0;
            try {
                uncompressedLength = DecodeVarInt(frame, lenBytes);
            } catch (const std::exception& e) {
                Log::Error("[%s] Bad compressed frame header: %s", m_name.c_str(), e.what());
                Disconnect();
                return false;
            }
            if (uncompressedLength == 0) {
                body     = frame + lenBytes;
                bodySize = frameSize - lenBytes;
            } else {
                if (uncompressedLength > MAX_PACKET_SIZE) {
                    Log::Error("[%s] Compressed frame claims %u bytes", m_name.c_str(),
                               uncompressedLength);
                    Disconnect();
                    return false;
                }
                inflated.resize(uncompressedLength);
                uLongf out = uncompressedLength;
                const int rc = uncompress(inflated.data(), &out,
                                          frame + lenBytes,
                                          static_cast<uLong>(frameSize - lenBytes));
                if (rc != Z_OK || out != uncompressedLength) {
                    Log::Error("[%s] Inflate failed (rc=%d, got %lu of %u)",
                               m_name.c_str(), rc, static_cast<unsigned long>(out),
                               uncompressedLength);
                    Disconnect();
                    return false;
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
            Log::Error("[%s] Failed to decode packet ID: %s", m_name.c_str(), e.what());
            Disconnect();
            return false;
        }

        if (packetId > 255 || packetIdBytes > bodySize) {
            Log::Error("[%s] Invalid packet ID: 0x%X", m_name.c_str(), packetId);
            Disconnect();
            return false;
        }

        m_currentPacket.header.length   = static_cast<uint32_t>(frameSize);
        m_currentPacket.header.packetId = static_cast<uint8_t>(packetId);

        // Extract payload (remaining bytes after VarInt packet ID)
        m_currentPacket.payload.assign(body + packetIdBytes, body + bodySize);

        m_stats.packetsReceived.fetch_add(1);

        // Decode packet on I/O thread (creates typed packet)
        try {
            PacketPtr packet = DecodePacket(m_currentPacket.header.packetId, m_currentPacket.payload);
            if (packet) {
                // Queue for main thread processing
                IncomingPacket incoming(std::move(packet));
                // try_push cannot fail — the queue is unbounded, exactly as
                // MC's PacketProcessor.packetsToBeHandled is. This used to
                // drop on a full 2048-slot queue, which is silent corruption
                // of a reliable stream. See MessageQueue.hpp.
                m_incomingPackets.try_push(std::move(incoming));
            } else if (ShouldDeferPacket(m_currentPacket.header.packetId)) {
                // Not decoded into a typed packet, but it still must not run
                // here — this is the network I/O thread. Queue it so the legacy
                // registry handler runs on the owning thread, which is MC's
                // PacketUtils.ensureRunningOnSameThread ->
                // PacketProcessor.scheduleIfPossible path.
                IncomingPacket incoming(std::make_unique<RawPayloadPacket>(
                    m_currentPacket.header.packetId, m_currentPacket.payload));
                m_incomingPackets.try_push(std::move(incoming));
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
            return false;
        }
        return true;
    }

    void NetworkConnection::ProcessSendQueue() {
        // Coalesce: everything queued goes out in ONE write, up to
        // kMaxWriteBytes. This replaced one async_write per packet — a
        // make_shared, a strand hop and a syscall each — which with a hundred
        // thousand tracked entities was the sender's ceiling, not the wire.
        //
        // A packet with a post-write hook ends its batch: the hook may reframe
        // the stream (compression), and "everything after this packet is
        // framed under the new rules" only holds if nothing after it was
        // framed before the hook ran.
        struct SendBatch {
            std::vector<uint8_t> bytes;
            std::function<void()> onSent;   // at most one, for the LAST frame
        };
        auto batch = std::make_shared<SendBatch>();

        std::vector<PendingSend> taken;
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (m_sendQueue.empty()) {
                m_sending = false;
                return;
            }
            size_t total = 0;
            while (!m_sendQueue.empty() && total < kMaxWriteBytes) {
                total += m_sendQueue.front().data.size();
                taken.push_back(std::move(m_sendQueue.front()));
                m_sendQueue.pop_front();
                if (taken.back().onSent) break;
            }
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
        for (PendingSend& e : taken) {
            const std::vector<uint8_t> framed = FrameForWire(e.data);
            batch->bytes.insert(batch->bytes.end(), framed.begin(), framed.end());
            if (e.onSent) batch->onSent = std::move(e.onSent);
        }

        // Async write - data is kept alive by the shared_ptr captured in lambda
        net::async_write(m_socket,
            net::buffer(batch->bytes),
            net::bind_executor(m_strand,
                [self = shared_from_this(), batch](const error_code& ec, size_t bytes) {
                    // The hook runs BEFORE HandleWrite queues the next batch,
                    // and before the error path, so a hook that reframes the
                    // stream (compression) takes effect for everything after
                    // this packet and nothing before it. Unconditional, as in
                    // PacketSendListener.thenRun.
                    if (batch->onSent) batch->onSent();
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