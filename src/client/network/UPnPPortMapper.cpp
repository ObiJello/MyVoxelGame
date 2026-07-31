// File: src/client/network/UPnPPortMapper.cpp
#include "UPnPPortMapper.hpp"
#include "common/network/AsioInclude.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <vector>

namespace Client {

    namespace {

        constexpr const char* kSsdpAddress = "239.255.255.250";
        constexpr uint16_t kSsdpPort = 1900;
        constexpr const char* kDescription = "ObeyCraft";

        // Service types to look for, most preferred first.
        const char* kServiceTypes[] = {
            "urn:schemas-upnp-org:service:WANIPConnection:1",
            "urn:schemas-upnp-org:service:WANIPConnection:2",
            "urn:schemas-upnp-org:service:WANPPPConnection:1",
        };

        std::string ToLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            return s;
        }

        // Pulls "<tag>value</tag>" starting the search at `from`.
        std::string ExtractTag(const std::string& xml, const std::string& tag,
                               size_t from = 0) {
            const std::string open = "<" + tag + ">";
            const std::string close = "</" + tag + ">";
            size_t a = xml.find(open, from);
            if (a == std::string::npos) return "";
            a += open.size();
            size_t b = xml.find(close, a);
            if (b == std::string::npos) return "";
            std::string value = xml.substr(a, b - a);
            // Trim whitespace/newlines that pretty-printed XML leaves behind.
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.erase(value.begin());
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.pop_back();
            return value;
        }

        // http://host:port/path  →  (host, port, path)
        bool SplitUrl(const std::string& url, std::string& host,
                      uint16_t& port, std::string& path) {
            const std::string prefix = "http://";
            if (url.compare(0, prefix.size(), prefix) != 0) return false;
            size_t hostStart = prefix.size();
            size_t pathStart = url.find('/', hostStart);
            std::string hostPort = (pathStart == std::string::npos)
                ? url.substr(hostStart)
                : url.substr(hostStart, pathStart - hostStart);
            path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
            size_t colon = hostPort.rfind(':');
            if (colon != std::string::npos) {
                host = hostPort.substr(0, colon);
                port = static_cast<uint16_t>(std::atoi(hostPort.c_str() + colon + 1));
            } else {
                host = hostPort;
                port = 80;
            }
            return !host.empty() && port != 0;
        }

        // Minimal blocking HTTP/1.1 exchange with a hard timeout. Returns the
        // full response (headers + body); the caller only cares about the body
        // content, which we locate by the blank-line separator.
        bool HttpExchange(const std::string& host, uint16_t port,
                          const std::string& request, std::string& outResponse,
                          int timeoutSeconds, std::string* outLocalIp = nullptr) {
            try {
                net::io_context io;
                net::ip::tcp::socket socket(io);
                net::ip::tcp::resolver resolver(io);
                auto endpoints = resolver.resolve(host, std::to_string(port));

                bool connected = false;
                net::async_connect(socket, endpoints,
                    [&](const error_code& ec, const net::ip::tcp::endpoint&) {
                        connected = !ec;
                    });
                io.run_for(std::chrono::seconds(timeoutSeconds));
                if (!connected) return false;

                if (outLocalIp) {
                    error_code ec;
                    auto local = socket.local_endpoint(ec);
                    if (!ec) *outLocalIp = local.address().to_string();
                }

                io.restart();
                bool wrote = false;
                net::async_write(socket, net::buffer(request),
                    [&](const error_code& ec, std::size_t) { wrote = !ec; });
                io.run_for(std::chrono::seconds(timeoutSeconds));
                if (!wrote) return false;

                // Read until the peer closes (these responses are small and
                // routers reliably close after them).
                io.restart();
                auto buffer = std::make_shared<std::array<char, 4096>>();
                outResponse.clear();
                std::function<void()> readMore = [&]() {
                    socket.async_read_some(net::buffer(*buffer),
                        [&](const error_code& ec, std::size_t n) {
                            if (ec) return;
                            outResponse.append(buffer->data(), n);
                            readMore();
                        });
                };
                readMore();
                io.run_for(std::chrono::seconds(timeoutSeconds));
                return !outResponse.empty();
            } catch (const std::exception&) {
                return false;
            }
        }

    } // namespace

    // ── SSDP discovery ──────────────────────────────────────────────────────

    bool UPnPPortMapper::Discover() {
        std::vector<std::string> locations;
        try {
            net::io_context io;
            net::ip::udp::socket socket(io, net::ip::udp::endpoint(net::ip::udp::v4(), 0));
            socket.set_option(net::ip::multicast::hops(2));

            const net::ip::udp::endpoint target(
                net::ip::make_address(kSsdpAddress), kSsdpPort);

            // Ask for the IGD root device; routers answer with a LOCATION
            // pointing at their description XML.
            for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                   "ssdp:all"}) {
                std::string msearch =
                    "M-SEARCH * HTTP/1.1\r\n"
                    "HOST: 239.255.255.250:1900\r\n"
                    "MAN: \"ssdp:discover\"\r\n"
                    "MX: 2\r\n"
                    "ST: " + std::string(st) + "\r\n\r\n";
                error_code ec;
                socket.send_to(net::buffer(msearch), target, 0, ec);
            }

            auto buffer = std::make_shared<std::array<char, 2048>>();
            auto sender = std::make_shared<net::ip::udp::endpoint>();
            std::function<void()> receiveMore = [&]() {
                socket.async_receive_from(net::buffer(*buffer), *sender,
                    [&](const error_code& ec, std::size_t n) {
                        if (ec || n == 0) return;
                        std::string response(buffer->data(), n);
                        // LOCATION header (case varies by vendor).
                        std::string lower = ToLower(response);
                        size_t pos = lower.find("location:");
                        if (pos != std::string::npos) {
                            size_t start = pos + 9;
                            size_t end = response.find("\r\n", start);
                            std::string url = response.substr(start, end - start);
                            while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front())))
                                url.erase(url.begin());
                            while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back())))
                                url.pop_back();
                            if (!url.empty() &&
                                std::find(locations.begin(), locations.end(), url) == locations.end()) {
                                locations.push_back(url);
                            }
                        }
                        receiveMore();
                    });
            };
            receiveMore();
            io.run_for(std::chrono::seconds(3));
        } catch (const std::exception& e) {
            Log::Info("[UPnP] discovery unavailable: %s", e.what());
            return false;
        }

        if (locations.empty()) return false;

        // Fetch each description until one exposes a WAN connection service.
        for (const auto& location : locations) {
            std::string host, path;
            uint16_t port = 0;
            if (!SplitUrl(location, host, port, path)) continue;

            const std::string request =
                "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                std::to_string(port) + "\r\nConnection: close\r\n\r\n";
            std::string response;
            if (!HttpExchange(host, port, request, response, 3)) continue;

            for (const char* wanted : kServiceTypes) {
                size_t pos = response.find(wanted);
                if (pos == std::string::npos) continue;
                // controlURL lives in the same <service> block, after the type.
                std::string controlUrl = ExtractTag(response, "controlURL", pos);
                if (controlUrl.empty()) continue;

                if (controlUrl.compare(0, 7, "http://") == 0) {
                    if (!SplitUrl(controlUrl, m_controlHost, m_controlPort, m_controlPath))
                        continue;
                } else {
                    m_controlHost = host;
                    m_controlPort = port;
                    m_controlPath = controlUrl.front() == '/' ? controlUrl
                                                              : "/" + controlUrl;
                }
                m_serviceType = wanted;
                Log::Info("[UPnP] gateway at %s:%u (%s)", m_controlHost.c_str(),
                          static_cast<unsigned>(m_controlPort), m_serviceType.c_str());
                return true;
            }
        }
        return false;
    }

    // ── SOAP ────────────────────────────────────────────────────────────────

    bool UPnPPortMapper::SoapCall(const std::string& action,
                                  const std::string& bodyArgs,
                                  std::string& outResponse) {
        const std::string body =
            "<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:" + action + " xmlns:u=\"" + m_serviceType + "\">" +
            bodyArgs +
            "</u:" + action + "></s:Body></s:Envelope>";

        const std::string request =
            "POST " + m_controlPath + " HTTP/1.1\r\n"
            "Host: " + m_controlHost + ":" + std::to_string(m_controlPort) + "\r\n"
            "Content-Type: text/xml; charset=\"utf-8\"\r\n"
            "SOAPAction: \"" + m_serviceType + "#" + action + "\"\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;

        std::string localIp;
        if (!HttpExchange(m_controlHost, m_controlPort, request, outResponse, 4,
                          &localIp)) {
            return false;
        }
        if (m_localIp.empty()) m_localIp = localIp;
        // Routers answer 500 + a UPnPError body when they refuse.
        return outResponse.find(" 200 ") != std::string::npos;
    }

    // ── public API ──────────────────────────────────────────────────────────

    UPnPPortMapper::Result UPnPPortMapper::Map(uint16_t port) {
        Result result;

        if (m_serviceType.empty() && !Discover()) {
            result.error = "no UPnP gateway found";
            Log::Info("[UPnP] no gateway responded — friends will be relayed");
            return result;
        }

        // Learn our LAN address (needed as NewInternalClient) by touching the
        // gateway once; GetExternalIPAddress doubles as that probe.
        std::string externalResponse;
        if (SoapCall("GetExternalIPAddress", "", externalResponse)) {
            result.externalIp = ExtractTag(externalResponse, "NewExternalIPAddress");
        }

        auto buildArgs = [&](int leaseSeconds) {
            return
                "<NewRemoteHost></NewRemoteHost>"
                "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>"
                "<NewProtocol>TCP</NewProtocol>"
                "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort>"
                "<NewInternalClient>" + m_localIp + "</NewInternalClient>"
                "<NewEnabled>1</NewEnabled>"
                "<NewPortMappingDescription>" + std::string(kDescription) +
                "</NewPortMappingDescription>"
                "<NewLeaseDuration>" + std::to_string(leaseSeconds) +
                "</NewLeaseDuration>";
        };

        if (m_localIp.empty()) {
            result.error = "could not determine LAN address";
            return result;
        }

        std::string response;
        // Permanent lease first; some routers reject 0 and require a finite
        // lease, so retry with an hour before giving up.
        bool mapped = SoapCall("AddPortMapping", buildArgs(0), response) ||
                      SoapCall("AddPortMapping", buildArgs(3600), response);
        if (!mapped) {
            result.error = "router refused the port mapping";
            Log::Info("[UPnP] AddPortMapping refused — friends will be relayed");
            return result;
        }

        m_mappedPort = port;
        result.ok = true;
        result.externalPort = port;
        Log::Info("[UPnP] mapped TCP %u -> %s:%u (external IP %s)",
                  static_cast<unsigned>(port), m_localIp.c_str(),
                  static_cast<unsigned>(port),
                  result.externalIp.empty() ? "unknown" : result.externalIp.c_str());
        return result;
    }

    void UPnPPortMapper::Unmap() {
        if (m_mappedPort == 0 || m_serviceType.empty()) return;
        const std::string args =
            "<NewRemoteHost></NewRemoteHost>"
            "<NewExternalPort>" + std::to_string(m_mappedPort) + "</NewExternalPort>"
            "<NewProtocol>TCP</NewProtocol>";
        std::string response;
        SoapCall("DeletePortMapping", args, response);
        Log::Info("[UPnP] removed mapping for TCP %u",
                  static_cast<unsigned>(m_mappedPort));
        m_mappedPort = 0;
    }

} // namespace Client
