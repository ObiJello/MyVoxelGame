// File: src/common/network/NetworkConnection.cpp
#include "NetworkConnection.hpp"
#include "../core/Log.hpp"
#include <algorithm>

namespace Network {

    std::atomic<uint32_t> NetworkConnection::s_nextConnectionId{1};

    NetworkConnection::NetworkConnection(tcp::socket socket)
        : m_socket(std::move(socket))
        , m_strand(boost::asio::make_strand(m_socket.get_executor()))
        , m_connectionId(s_nextConnectionId.fetch_add(1))
        , m_name("Connection#" + std::to_string(m_connectionId))
    {
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
            
            boost::system::error_code ec;
            m_socket.shutdown(tcp::socket::shutdown_both, ec);
            m_socket.close(ec);
            
            m_state = ConnectionState::DISCONNECTED;
            OnDisconnected();
        }
    }

    void NetworkConnection::Disconnect() {
        if (m_state.exchange(ConnectionState::DISCONNECTED) != ConnectionState::DISCONNECTED) {
            Log::Info("[%s] Force disconnecting", m_name.c_str());
            
            boost::system::error_code ec;
            m_socket.close(ec);
            
            OnDisconnected();
        }
    }

    void NetworkConnection::SendPacket(uint8_t packetId, const std::vector<uint8_t>& data) {
        // Create packet with header
        std::vector<uint8_t> packet;
        
        // Reserve space for VarInt length (max 5 bytes) + VarInt packet ID (max 5 bytes) + payload
        packet.reserve(10 + data.size());
        
        // Encode packet ID as VarInt to temp buffer
        std::vector<uint8_t> packetIdBytes;
        EncodeVarInt(static_cast<uint32_t>(packetId), packetIdBytes);
        
        // Calculate packet size (VarInt packet ID + payload)
        uint32_t packetSize = static_cast<uint32_t>(packetIdBytes.size() + data.size());
        
        // Encode length as VarInt
        EncodeVarInt(packetSize, packet);
        
        // Add packet ID as VarInt
        packet.insert(packet.end(), packetIdBytes.begin(), packetIdBytes.end());
        
        // Add payload
        packet.insert(packet.end(), data.begin(), data.end());
        
        // Debug logging
        if (packetId == 0x20 || packetId == 0x81) {  // ChunkDataS2C or suspicious 0x81
            Log::Info("[%s] SENDING packet ID 0x%02X, packetIdBytes size: %zu, payload size: %zu, total size: %zu", 
                      m_name.c_str(), packetId, packetIdBytes.size(), data.size(), packet.size());
            
            // Log first few bytes of packet for debugging
            std::string hexDump;
            for (size_t i = 0; i < std::min(size_t(20), packet.size()); i++) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X ", packet[i]);
                hexDump += buf;
            }
            Log::Debug("[%s] Packet hex (first 20 bytes): %s", m_name.c_str(), hexDump.c_str());
        }
        
        // Send the packet
        SendRaw(packet);
        
        m_stats.packetsSent.fetch_add(1);
    }

    void NetworkConnection::SendPacket(const RawPacket& packet) {
        SendPacket(packet.header.packetId, packet.payload);
    }

    void NetworkConnection::SendRaw(const std::vector<uint8_t>& data) {
        if (m_state != ConnectionState::CONNECTED) {
            Log::Warning("[%s] Attempted to send data on disconnected connection", m_name.c_str());
            return;
        }
        
        // Check queue size limit
        size_t queueSize = 0;
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            for (const auto& packet : m_sendQueue) {
                queueSize += packet.size();
            }
        }
        
        if (queueSize + data.size() > MAX_SEND_QUEUE_SIZE) {
            Log::Error("[%s] Send queue full, dropping packet", m_name.c_str());
            return;
        }
        
        // Add to send queue
        bool startSend = false;
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            m_sendQueue.push_back(data);
            if (!m_sending) {
                m_sending = true;
                startSend = true;
            }
        }
        
        // Start async send if not already sending
        if (startSend) {
            boost::asio::post(m_strand, [self = shared_from_this()]() {
                self->ProcessSendQueue();
            });
        }
        
        m_stats.bytesSent.fetch_add(data.size());
    }

    void NetworkConnection::StartRead() {
        if (m_state != ConnectionState::CONNECTED) {
            return;
        }
        
        Log::Debug("[%s] Starting async read chain", m_name.c_str());
        
        // Reset read state for new packet
        m_readingHeader = true;
        m_readPos = 0;
        m_currentPacket = RawPacket();
        
        // Start async read for VarInt length (1-5 bytes)
        boost::asio::async_read(m_socket,
            boost::asio::buffer(m_readBuffer.data(), 1),
            boost::asio::bind_executor(m_strand,
                [self = shared_from_this()](const boost::system::error_code& ec, size_t bytes) {
                    self->HandleReadHeader(ec, bytes);
                }));
    }

    void NetworkConnection::HandleReadHeader(const boost::system::error_code& error, size_t bytesTransferred) {
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
            
            boost::asio::async_read(m_socket,
                boost::asio::buffer(m_readBuffer.data() + m_readPos, 1),
                boost::asio::bind_executor(m_strand,
                    [self = shared_from_this()](const boost::system::error_code& ec, size_t bytes) {
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
        
        boost::asio::async_read(m_socket,
            boost::asio::buffer(m_readBuffer.data(), packetLength),
            boost::asio::bind_executor(m_strand,
                [self = shared_from_this()](const boost::system::error_code& ec, size_t bytes) {
                    self->HandleReadPayload(ec, bytes);
                }));
    }

    void NetworkConnection::HandleReadPayload(const boost::system::error_code& error, size_t bytesTransferred) {
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
        
        // Extract packet ID as VarInt
        size_t packetIdBytes = 0;
        uint32_t packetId = 0;
        try {
            packetId = DecodeVarInt(m_readBuffer.data(), packetIdBytes);
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
        m_currentPacket.payload.assign(
            m_readBuffer.begin() + packetIdBytes,
            m_readBuffer.begin() + bytesTransferred
        );
        
        // Debug log for received packets
        if (m_currentPacket.header.packetId == 0x20 || m_currentPacket.header.packetId == 0x81) {
            Log::Info("[%s] RECEIVED packet ID 0x%02X, packetIdBytes: %zu, payload size: %zu, raw: %s", 
                      m_name.c_str(), m_currentPacket.header.packetId, packetIdBytes, 
                      m_currentPacket.payload.size(), hexDump.c_str());
        }
        
        m_stats.packetsReceived.fetch_add(1);
        
        // Decode packet on I/O thread (creates typed packet)
        try {
            PacketPtr packet = DecodePacket(m_currentPacket.header.packetId, m_currentPacket.payload);
            if (packet) {
                // Queue for main thread processing
                IncomingPacket incoming(std::move(packet));
                if (!m_incomingPackets.try_push(std::move(incoming))) {
                    Log::Warning("[%s] Incoming packet queue full, dropping packet ID 0x%02X", 
                                m_name.c_str(), m_currentPacket.header.packetId);
                }
            } else {
                // Fallback to legacy callback for backward compatibility
                OnPacketReceived(m_currentPacket.header.packetId, m_currentPacket.payload);
            }
        } catch (const std::exception& e) {
            Log::Error("[%s] Exception decoding packet: %s", m_name.c_str(), e.what());
        }
        
        // Start reading next packet
        StartRead();
    }

    void NetworkConnection::ProcessSendQueue() {
        std::vector<uint8_t> data;
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (m_sendQueue.empty()) {
                m_sending = false;
                return;
            }
            data = std::move(m_sendQueue.front());
            m_sendQueue.pop_front();
        }
        
        // Async write
        boost::asio::async_write(m_socket,
            boost::asio::buffer(data),
            boost::asio::bind_executor(m_strand,
                [self = shared_from_this(), dataSize = data.size()](const boost::system::error_code& ec, size_t bytes) {
                    self->HandleWrite(ec, bytes);
                }));
    }

    void NetworkConnection::HandleWrite(const boost::system::error_code& error, size_t bytesTransferred) {
        if (error) {
            HandleError(error);
            return;
        }
        
        Log::Debug("[%s] Successfully wrote %zu bytes to socket", m_name.c_str(), bytesTransferred);
        
        // Continue processing send queue
        ProcessSendQueue();
    }

    void NetworkConnection::HandleError(const boost::system::error_code& error) {
        if (error == boost::asio::error::eof || 
            error == boost::asio::error::connection_reset ||
            error == boost::asio::error::broken_pipe) {
            Log::Info("[%s] Connection closed by peer", m_name.c_str());
        } else if (error != boost::asio::error::operation_aborted) {
            Log::Error("[%s] Network error: %s", m_name.c_str(), error.message().c_str());
        }
        
        OnError(error);
        Disconnect();
    }

    boost::asio::ip::tcp::endpoint NetworkConnection::GetRemoteEndpoint() const {
        boost::system::error_code ec;
        return m_socket.remote_endpoint(ec);
    }

    boost::asio::ip::tcp::endpoint NetworkConnection::GetLocalEndpoint() const {
        boost::system::error_code ec;
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