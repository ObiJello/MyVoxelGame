// File: src/client/network/UPnPPortMapper.hpp
//
// Opens the router's port for the integrated server so friends can join a
// hosted world without anyone touching router settings. Speaks UPnP IGD
// (the same mechanism consoles and torrent clients use):
//
//   1. SSDP M-SEARCH over UDP multicast to find the gateway
//   2. HTTP GET its device description XML
//   3. locate the WAN{IP,PPP}Connection service's control URL
//   4. SOAP AddPortMapping  (+ GetExternalIPAddress)
//
// Hand-rolled on Asio (already vendored) rather than pulling in miniupnpc —
// avoids a new C dependency in the universal/cross-platform build, and
// failure is harmless here: the friends service verifies reachability
// independently and falls back to relaying, so a router that ignores UPnP
// just means that player gets relayed.
//
// Every call BLOCKS (bounded by short timeouts). Call from a worker thread,
// never from the render or io threads.
#pragma once

#include <cstdint>
#include <string>

namespace Client {

    class UPnPPortMapper {
    public:
        struct Result {
            bool ok = false;
            uint16_t externalPort = 0;   // what friends should dial
            std::string externalIp;      // router's WAN address ("" if unknown)
            std::string error;           // human-readable, for logs
        };

        // Discovers a gateway and maps external `port` → this machine's
        // `port` (TCP). Safe to call repeatedly; routers overwrite an
        // identical mapping. Worst case ~6 seconds.
        Result Map(uint16_t port);

        // Removes the mapping created by Map(). Best-effort and quick;
        // safe to call when Map() failed or was never called.
        void Unmap();

    private:
        // Gateway details cached between Map and Unmap.
        std::string m_controlHost;
        uint16_t    m_controlPort = 0;
        std::string m_controlPath;
        std::string m_serviceType;
        std::string m_localIp;
        uint16_t    m_mappedPort = 0;

        // Retries DiscoverOnce; see the definition for why a single SSDP
        // sweep is not reliable enough to decide the whole session on.
        bool Discover();
        bool DiscoverOnce();
        bool SoapCall(const std::string& action, const std::string& bodyArgs,
                      std::string& outResponse);
    };

} // namespace Client
