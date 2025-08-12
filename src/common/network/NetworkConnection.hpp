// File: src/common/network/NetworkConnection.hpp
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <functional>
#include "IPacket.hpp"
#include "MessageQueue.hpp"

namespace Network {

    // Forward declarations
    class NetworkConnection;
    using ConnectionPtr = std::shared_ptr<NetworkConnection>;
    
    // Packet header structure
    struct PacketHeader {
        uint32_t length;    // VarInt encoded in actual transmission
        uint8_t packetId;
    };

    // Raw packet data
    struct RawPacket {
        PacketHeader header;
        std::vector<uint8_t> payload;
    };

    // Connection state
    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        HANDSHAKING,
        CONNECTED,
        DISCONNECTING
    };

    // Abstract base class for network connections (similar to Netty's Channel)
    // Both client and server connections inherit from this
    class NetworkConnection : public std::enable_shared_from_this<NetworkConnection> {
    public:
        using tcp = boost::asio::ip::tcp;
        
        // Constructor takes ownership of socket
        explicit NetworkConnection(tcp::socket socket);
        virtual ~NetworkConnection();

        // Non-copyable, non-movable
        NetworkConnection(const NetworkConnection&) = delete;
        NetworkConnection& operator=(const NetworkConnection&) = delete;

        // ========================================================================
        // CONNECTION LIFECYCLE
        // ========================================================================

        // Start async read/write operations
        virtual void Start();
        
        // Close the connection gracefully
        virtual void Close();
        
        // Force disconnect
        virtual void Disconnect();
        
        // Check if connected
        bool IsConnected() const { return m_state == ConnectionState::CONNECTED; }
        ConnectionState GetState() const { return m_state.load(); }

        // ========================================================================
        // PACKET I/O
        // ========================================================================

        // Send a packet asynchronously (thread-safe, queues to write strand)
        void SendPacket(uint8_t packetId, const std::vector<uint8_t>& data);
        void SendPacket(const RawPacket& packet);
        
        // Send raw bytes (for handshake/protocol negotiation)
        void SendRaw(const std::vector<uint8_t>& data);
        
        // Try to pop an incoming packet from the queue (main thread)
        bool TryPopIncoming(IncomingPacket& packet) {
            return m_incomingPackets.try_pop(packet);
        }
        
        // Get incoming queue stats
        size_t GetIncomingQueueSize() const { return m_incomingPackets.Size(); }
        size_t GetDroppedPacketCount() const { return m_incomingPackets.GetDroppedCount(); }

        // ========================================================================
        // CALLBACKS (OVERRIDE IN DERIVED CLASSES)
        // ========================================================================

        // Decode packet on I/O thread (override in derived class to create typed packets)
        virtual PacketPtr DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) = 0;
        
        // Legacy callback for compatibility (will be removed)
        virtual void OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) {}
        
        // Called when connection is established
        virtual void OnConnected() {}
        
        // Called when connection is lost
        virtual void OnDisconnected() {}
        
        // Called on error
        virtual void OnError(const boost::system::error_code& error) {}

        // ========================================================================
        // CONNECTION INFO
        // ========================================================================

        // Get remote endpoint
        tcp::endpoint GetRemoteEndpoint() const;
        
        // Get local endpoint
        tcp::endpoint GetLocalEndpoint() const;
        
        // Get connection ID (for logging)
        uint32_t GetConnectionId() const { return m_connectionId; }
        
        // Get/set connection name (for debugging)
        void SetName(const std::string& name) { m_name = name; }
        const std::string& GetName() const { return m_name; }

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ConnectionStats {
            std::atomic<uint64_t> bytesSent{0};
            std::atomic<uint64_t> bytesReceived{0};
            std::atomic<uint64_t> packetsSent{0};
            std::atomic<uint64_t> packetsReceived{0};
            std::chrono::steady_clock::time_point connectedTime;
        };
        
        const ConnectionStats& GetStats() const { return m_stats; }

    protected:
        // ========================================================================
        // ASYNC OPERATIONS (INTERNAL)
        // ========================================================================

        // Start async read for packet header
        void StartRead();
        
        // Handle header read completion
        void HandleReadHeader(const boost::system::error_code& error, size_t bytesTransferred);
        
        // Handle payload read completion
        void HandleReadPayload(const boost::system::error_code& error, size_t bytesTransferred);
        
        // Process send queue
        void ProcessSendQueue();
        
        // Handle write completion
        void HandleWrite(const boost::system::error_code& error, size_t bytesTransferred);
        
        // Handle errors
        void HandleError(const boost::system::error_code& error);

        // ========================================================================
        // VARINT ENCODING/DECODING
        // ========================================================================

        // Encode integer as VarInt
        static void EncodeVarInt(uint32_t value, std::vector<uint8_t>& buffer);
        
        // Decode VarInt from buffer
        static uint32_t DecodeVarInt(const uint8_t* data, size_t& bytesRead);
        
        // Get VarInt size
        static size_t GetVarIntSize(uint32_t value);

    protected:
        // Socket and strand for thread safety
        tcp::socket m_socket;
        boost::asio::any_io_executor m_strand;
        
        // Incoming packet queue (thread-safe, written by I/O thread, read by main thread)
        MessageQueue<IncomingPacket> m_incomingPackets{2048}; // Allow up to 2048 packets queued
        
        // Connection state
        std::atomic<ConnectionState> m_state{ConnectionState::DISCONNECTED};
        
        // Read buffer
        std::vector<uint8_t> m_readBuffer;
        size_t m_readPos = 0;
        
        // Current packet being read
        RawPacket m_currentPacket;
        bool m_readingHeader = true;
        
        // Send queue (thread-safe)
        std::mutex m_sendMutex;
        std::deque<std::vector<uint8_t>> m_sendQueue;
        bool m_sending = false;
        
        // Connection info
        uint32_t m_connectionId;
        std::string m_name;
        
        // Statistics
        ConnectionStats m_stats;
        
        // Static connection ID counter
        static std::atomic<uint32_t> s_nextConnectionId;
        
        // Maximum packet size (2 MiB) - matches typical Minecraft cap
        static constexpr size_t MAX_PACKET_SIZE = 2 * 1024 * 1024;
    };

} // namespace Network