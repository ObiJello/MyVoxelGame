// File: src/client/sync/FileSyncService.cpp
#include "client/sync/FileSyncService.hpp"

#include "common/sync/FileSyncConfig.hpp"
#include "common/sync/FileDeltaSync.hpp"
#include "client/network/UPnPPortMapper.hpp"
#include "common/network/AsioInclude.hpp"
#include "common/core/Deflate.hpp"
#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Sync {

    namespace {
        namespace fs = std::filesystem;
        using tcp = net::ip::tcp;
        using Clock = std::chrono::steady_clock;
        using json = nlohmann::json;

        constexpr auto kIoTimeout      = std::chrono::seconds(60);   // per framed delta IO op
        constexpr auto kConnectTimeout = std::chrono::seconds(10);
        constexpr auto kDirectProbe    = std::chrono::seconds(3);    // direct connect budget
        constexpr auto kDiscoverWindow = std::chrono::seconds(30);   // client: wait for the source to offer per attempt
        constexpr auto kClientRetryGap = std::chrono::seconds(15);   // client: gap between whole-session retries
        constexpr auto kLinkPing       = std::chrono::seconds(20);   // source link keep-alive tick (< service 90s)
        constexpr uint64_t kFreeSpaceMargin = 64ull * 1024 * 1024;

        bool IEquals(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                    return false;
            return true;
        }

        std::string Timestamp() {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
            return buf;
        }

        bool MessagesRunning() {
#ifdef __APPLE__
            FILE* p = ::popen("pgrep -x Messages 2>/dev/null", "r");
            if (!p) return false;
            char buf[64];
            bool any = false;
            while (std::fgets(buf, sizeof(buf), p))
                if (buf[0] != '\0' && buf[0] != '\n') any = true;
            ::pclose(p);
            return any;
#else
            return false;
#endif
        }

        void SleepWithStop(FileSyncService& svc, std::chrono::milliseconds total) {
            const auto deadline = Clock::now() + total;
            while (!svc.Stopping() && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // ── Friendship ticket gate (source side) ─────────────────────────────
        // Tickets minted by the friends service's filesync_connect (and pushed
        // to the source) are the shared secret that gates the DIRECT path, so a
        // random internet scanner that finds the UPnP-mapped port cannot pull
        // the database. Short-lived.
        struct TicketGate {
            std::mutex m;
            std::vector<std::pair<std::string, Clock::time_point>> tickets;
            void Prune() {
                const auto now = Clock::now();
                tickets.erase(std::remove_if(tickets.begin(), tickets.end(),
                    [&](const auto& e) { return now - e.second > std::chrono::seconds(120); }),
                    tickets.end());
            }
            void Add(const std::string& t) {
                std::lock_guard<std::mutex> lk(m); Prune(); tickets.emplace_back(t, Clock::now());
            }
            bool Check(const std::string& t) {
                if (t.empty()) return false;
                std::lock_guard<std::mutex> lk(m); Prune();
                for (auto& e : tickets) if (e.first == t) return true;
                return false;
            }
            // The direct path can arrive before the service's relay push has
            // added the ticket. Wait briefly for it instead of rejecting.
            bool WaitFor(const std::string& t, std::chrono::milliseconds timeout) {
                const auto deadline = Clock::now() + timeout;
                for (;;) {
                    if (Check(t)) return true;
                    if (Clock::now() >= deadline) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        };

        // ── Framed delta IO (bounded timeouts) — unchanged protocol ──────────
        void DrainAfterFailure(net::io_context& io, tcp::socket& sock) {
            error_code ig; sock.close(ig); io.run();
        }

        bool SendFrame(net::io_context& io, tcp::socket& sock, MsgType type,
                       const std::vector<uint8_t>& payload) {
            std::vector<uint8_t> frame = EncodeFrame(type, payload);
            bool ok = false;
            io.restart();
            net::async_write(sock, net::buffer(frame),
                [&](const error_code& ec, std::size_t n) { ok = !ec && n == frame.size(); });
            io.run_for(kIoTimeout);
            if (!ok) { DrainAfterFailure(io, sock); return false; }
            return true;
        }

        bool ReadN(net::io_context& io, tcp::socket& sock, uint8_t* dst, size_t n) {
            if (n == 0) return true;
            bool ok = false;
            io.restart();
            net::async_read(sock, net::buffer(dst, n),
                [&](const error_code& ec, std::size_t got) { ok = !ec && got == n; });
            io.run_for(kIoTimeout);
            if (!ok) { DrainAfterFailure(io, sock); return false; }
            return true;
        }

        bool RecvFrame(net::io_context& io, tcp::socket& sock,
                       MsgType& outType, std::vector<uint8_t>& outPayload) {
            uint8_t header[5];
            if (!ReadN(io, sock, header, 5)) return false;
            MsgType type; uint32_t len;
            if (!DecodeHeader(header, type, len)) return false;
            outPayload.assign(len, 0);
            if (len && !ReadN(io, sock, outPayload.data(), len)) return false;
            uint8_t crcbuf[4];
            if (!ReadN(io, sock, crcbuf, 4)) return false;
            if (!VerifyCrc(type, outPayload.data(), len, GetU32(crcbuf))) return false;
            outType = type;
            return true;
        }

        // ── NDJSON control IO over the service socket ────────────────────────
        bool DialService(net::io_context& io, tcp::socket& sock,
                         const std::string& host, uint16_t port,
                         std::chrono::steady_clock::duration timeout) {
            tcp::resolver resolver(io);
            error_code rec;
            auto endpoints = resolver.resolve(host, std::to_string(port), rec);
            if (rec) return false;
            bool connected = false;
            io.restart();
            net::async_connect(sock, endpoints,
                [&](const error_code& ec, const tcp::endpoint&) { connected = !ec; });
            io.run_for(timeout);
            if (!connected) { DrainAfterFailure(io, sock); return false; }
            return true;
        }

        bool NdjsonWrite(net::io_context& io, tcp::socket& sock, const std::string& jsonNoNl) {
            std::string line = jsonNoNl + "\n";
            bool ok = false;
            io.restart();
            net::async_write(sock, net::buffer(line),
                [&](const error_code& ec, std::size_t n) { ok = !ec && n == line.size(); });
            io.run_for(kIoTimeout);
            if (!ok) { DrainAfterFailure(io, sock); return false; }
            return true;
        }

        enum class ReadRes { Got, TimedOut, Failed };

        // Reads one NDJSON line. On timeout the socket is left OPEN (the read is
        // cancelled) so the caller can ping and keep the link alive.
        ReadRes NdjsonRead(net::io_context& io, tcp::socket& sock,
                           net::streambuf& buf, std::string& outLine,
                           std::chrono::steady_clock::duration timeout) {
            bool done = false, ok = false;
            io.restart();
            net::async_read_until(sock, buf, '\n',
                [&](const error_code& ec, std::size_t) { done = true; ok = !ec; });
            io.run_for(timeout);
            if (!done) { error_code ig; sock.cancel(ig); io.run(); return ReadRes::TimedOut; }
            if (!ok) return ReadRes::Failed;
            std::istream is(&buf);
            std::getline(is, outLine);
            return ReadRes::Got;
        }

        // Reads a tiny line one byte at a time (no over-read into a buffer), so
        // the raw delta stream that follows a relay ack is left intact.
        bool ReadLineNoOverread(net::io_context& io, tcp::socket& sock,
                                std::string& out,
                                std::chrono::steady_clock::duration timeout) {
            out.clear();
            const auto deadline = Clock::now() + timeout;
            char c;
            while (Clock::now() < deadline) {
                bool done = false, ok = false;
                io.restart();
                net::async_read(sock, net::buffer(&c, 1),
                    [&](const error_code& ec, std::size_t n) { done = true; ok = !ec && n == 1; });
                io.run_for(deadline - Clock::now());
                if (!done) { error_code ig; sock.cancel(ig); io.run(); return false; }
                if (!ok) return false;
                if (c == '\n') return true;
                out.push_back(c);
                if (out.size() > 4096) return false;
            }
            return false;
        }

        bool ServiceHello(net::io_context& io, tcp::socket& sock, net::streambuf& buf,
                          const std::string& token) {
            json j{{"op", "hello"}, {"token", token}, {"id", 1}};
            if (!NdjsonWrite(io, sock, j.dump())) return false;
            std::string line;
            if (NdjsonRead(io, sock, buf, line, kConnectTimeout) != ReadRes::Got) return false;
            try { return json::parse(line).value("ok", false); }
            catch (...) { return false; }
        }

        // Attach half a relay tunnel. Reads the one-line ack byte-by-byte so no
        // spliced payload is swallowed.
        bool RelayAttach(net::io_context& io, tcp::socket& sock,
                         const std::string& role, const std::string& ticket) {
            json j{{"op", "relay_attach"}, {"role", role}, {"ticket", ticket}};
            if (!NdjsonWrite(io, sock, j.dump())) return false;
            std::string ack;
            if (!ReadLineNoOverread(io, sock, ack, kConnectTimeout)) return false;
            return ack.find("\"ok\":true") != std::string::npos;
        }

        // ── Serve side of the delta (over a connected, authed socket) ─────────
        void ServeDelta(net::io_context& io, tcp::socket& sock, const std::string& servePath,
                        const std::shared_ptr<TicketGate>& gate) {
            // Send the client a Refuse carrying the reason, so the failure
            // reason lands in the CLIENT's log (the other Mac), not just here.
            const auto refuse = [&](const std::string& reason) {
                Log::Warning("[FileSync] serve: %s", reason.c_str());
                std::vector<uint8_t> pl(reason.begin(), reason.end());
                SendFrame(io, sock, MsgType::Refuse, pl);
            };

            // Friendship-ticket gate (both the direct and relay client send it).
            {
                MsgType t; std::vector<uint8_t> pl;
                if (!RecvFrame(io, sock, t, pl) || t != MsgType::Hello) {
                    refuse("no hello frame (build/protocol mismatch between the two Macs?)"); return;
                }
                std::string ticket((const char*)pl.data(), pl.size());
                // Wait briefly: the direct path can beat the service's ticket push.
                if (!gate->WaitFor(ticket, std::chrono::seconds(5))) {
                    refuse("ticket not authorized (friendship-ticket not received from the service yet)"); return;
                }
            }

            if (MessagesRunning()) {
                std::string reason = "Messages is running on the source Mac — quit Messages, then it will serve chat.db.";
                Log::Warning("[FileSync] %s", reason.c_str());
                std::vector<uint8_t> pl(reason.begin(), reason.end());
                SendFrame(io, sock, MsgType::Refuse, pl);
                return;
            }
            {
                error_code ec;
                const std::string wal = servePath + "-wal";
                if (fs::exists(wal, ec) && fs::file_size(wal, ec) > 0)
                    Log::Warning("[FileSync] %s is non-empty — only chat.db itself is synced; "
                                 "quit Messages so it checkpoints into chat.db first", wal.c_str());
            }

            MappedFile mf;
            if (!mf.Open(servePath)) {
                std::string reason = "source cannot open " + servePath;
                Log::Error("[FileSync] %s", reason.c_str());
                std::vector<uint8_t> pl(reason.begin(), reason.end());
                SendFrame(io, sock, MsgType::Refuse, pl);
                return;
            }
            if (mf.size() > kMaxFileSize) { Log::Error("[FileSync] source file too large"); return; }

            MsgType t; std::vector<uint8_t> pl;
            if (!RecvFrame(io, sock, t, pl) || t != MsgType::Signatures || pl.size() < 16) {
                refuse("did not receive valid signatures (build/protocol mismatch?)"); return;
            }
            const uint32_t blockSize = GetU32(pl.data());
            const uint32_t numBlocks = GetU32(pl.data() + 12);
            if (blockSize == 0 || blockSize > (1u << 20) ||
                size_t(16) + size_t(numBlocks) * 20u != pl.size()) {
                refuse("malformed signature payload (build/protocol mismatch?)"); return;
            }
            std::vector<BlockSignature> sigs(numBlocks);
            for (uint32_t i = 0; i < numBlocks; ++i) {
                const uint8_t* q = pl.data() + 16 + size_t(i) * 20;
                sigs[i].weak = GetU32(q);
                sigs[i].strong = Hash128{ GetU64(q + 4), GetU64(q + 12) };
            }
            Log::Info("[FileSync] serving: %u client blocks, source %.1f MB", numBlocks, mf.size() / 1048576.0);

            Murmur3Incremental h;
            {
                const uint64_t kR = 1u << 20; uint64_t off = 0;
                while (off < mf.size()) {
                    const uint64_t take = std::min<uint64_t>(kR, mf.size() - off);
                    h.Update(mf.data() + off, (size_t)take); off += take;
                }
            }
            const Hash128 whole = h.Finish();
            {
                std::vector<uint8_t> info;
                PutU64(info, mf.size()); PutU64(info, whole.h1); PutU64(info, whole.h2);
                if (!SendFrame(io, sock, MsgType::FileInfo, info)) return;
            }

            bool abort = false;
            DeltaSink sink;
            sink.onCopy = [&](uint64_t baseOffset, uint64_t len) {
                if (abort) return;
                std::vector<uint8_t> p; PutU64(p, baseOffset); PutU64(p, len);
                if (!SendFrame(io, sock, MsgType::Copy, p)) abort = true;
            };
            sink.onLiteral = [&](const uint8_t* data, uint64_t len) {
                if (abort) return;
                std::vector<uint8_t> comp;
                bool ok = Core::Deflate::Compress(data, (size_t)len, comp, Core::Deflate::Format::Raw);
                std::vector<uint8_t> p;
                PutU64(p, len);
                if (ok && comp.size() < len) {
                    p.push_back(1); PutU32(p, (uint32_t)comp.size());
                    p.insert(p.end(), comp.begin(), comp.end());
                } else {
                    p.push_back(0); PutU32(p, (uint32_t)len);
                    p.insert(p.end(), data, data + len);
                }
                if (!SendFrame(io, sock, MsgType::Literal, p)) abort = true;
            };
            BuildDelta(mf.data(), mf.size(), blockSize, sigs, sink);
            if (abort) { Log::Warning("[FileSync] client disconnected during delta"); return; }
            SendFrame(io, sock, MsgType::End, {});

            if (RecvFrame(io, sock, t, pl) && t == MsgType::Result && !pl.empty())
                Log::Info("[FileSync] client reported %s", pl[0] ? "SUCCESS" : "failure");
        }

        // ── Pull side of the delta (over a connected, attached socket) ────────
        // Reconstructs <target>.incoming, verifies the whole-file hash, then
        // backs up + atomically swaps. Target is never written in place.
        bool PullDelta(net::io_context& io, FileSyncService& svc, tcp::socket& sock,
                       const std::string& targetPath, bool dryRun, const std::string& ticket) {
            // Present the friendship ticket (gates the direct path; harmless on relay).
            { std::vector<uint8_t> hp(ticket.begin(), ticket.end());
              if (!SendFrame(io, sock, MsgType::Hello, hp)) { Log::Error("[FileSync] failed to send hello"); return false; } }

            MappedFile base;
            base.Open(targetPath);   // invalid => full transfer
            std::vector<BlockSignature> sigs = ComputeSignatures(base);
            {
                std::vector<uint8_t> pl;
                PutU32(pl, kBlockSize);
                PutU64(pl, base.size());
                PutU32(pl, (uint32_t)sigs.size());
                for (const auto& s : sigs) { PutU32(pl, s.weak); PutU64(pl, s.strong.h1); PutU64(pl, s.strong.h2); }
                if (!SendFrame(io, sock, MsgType::Signatures, pl)) {
                    // The source closed right after the handshake. Try to read a
                    // Refuse it may have sent, so the real reason surfaces instead
                    // of a bare "failed to send signatures".
                    MsgType rt; std::vector<uint8_t> rpl;
                    if (RecvFrame(io, sock, rt, rpl) && rt == MsgType::Refuse)
                        Log::Warning("[FileSync] source refused: %.*s", (int)rpl.size(), (const char*)rpl.data());
                    else
                        Log::Error("[FileSync] source closed after the handshake — quit Messages on the "
                                   "source Mac, and make sure both Macs run the same build");
                    return false;
                }
            }
            Log::Info("[FileSync] sent %zu block signatures (base %.1f MB)", sigs.size(), base.size() / 1048576.0);

            MsgType t; std::vector<uint8_t> pl;
            if (!RecvFrame(io, sock, t, pl)) { Log::Error("[FileSync] no reply to signatures"); return false; }
            if (t == MsgType::Refuse) {
                Log::Warning("[FileSync] source refused: %.*s", (int)pl.size(), (const char*)pl.data());
                return false;
            }
            if (t != MsgType::FileInfo || pl.size() < 24) { Log::Error("[FileSync] bad FileInfo"); return false; }
            const uint64_t newSize = GetU64(pl.data());
            Hash128 wholeHash{ GetU64(pl.data() + 8), GetU64(pl.data() + 16) };
            if (newSize == 0 || newSize > kMaxFileSize) { Log::Error("[FileSync] implausible file size"); return false; }
            Log::Info("[FileSync] source file is %.1f MB; reconstructing", newSize / 1048576.0);

            const fs::path target = fs::path(targetPath);
            const fs::path parent = target.parent_path();
            error_code dec;
            if (!parent.empty()) fs::create_directories(parent, dec);
            {
                error_code sec;
                auto sp = fs::space(parent.empty() ? fs::path(".") : parent, sec);
                if (!sec && sp.available < newSize + kFreeSpaceMargin) {
                    Log::Error("[FileSync] not enough free space (%.1f MB avail, need %.1f MB)",
                               sp.available / 1048576.0, (newSize + kFreeSpaceMargin) / 1048576.0);
                    return false;
                }
            }

            const std::string incomingPath = targetPath + ".incoming";
            std::ofstream out(incomingPath, std::ios::binary | std::ios::trunc);
            if (!out) { Log::Error("[FileSync] cannot open %s for writing", incomingPath.c_str()); return false; }

            Murmur3Incremental hasher;
            uint64_t written = 0, copiedBytes = 0, literalBytes = 0;
            std::vector<uint8_t> litRaw;
            bool failed = false;

            auto writeChunks = [&](const uint8_t* data, uint64_t len) {
                const uint64_t kW = 1u << 20; uint64_t off = 0;
                while (off < len) {
                    const uint64_t take = std::min<uint64_t>(kW, len - off);
                    out.write((const char*)data + off, (std::streamsize)take);
                    hasher.Update(data + off, (size_t)take);
                    off += take;
                }
            };

            for (;;) {
                if (svc.Stopping()) { failed = true; break; }
                if (!RecvFrame(io, sock, t, pl)) { failed = true; break; }
                if (t == MsgType::End) break;
                if (t == MsgType::Copy) {
                    if (pl.size() < 16) { failed = true; break; }
                    const uint64_t off = GetU64(pl.data());
                    const uint64_t len = GetU64(pl.data() + 8);
                    if (!base.valid() || off + len > base.size() || written + len > newSize) { failed = true; break; }
                    writeChunks(base.data() + off, len);
                    written += len; copiedBytes += len;
                } else if (t == MsgType::Literal) {
                    if (pl.size() < 13) { failed = true; break; }
                    const uint64_t rawLen = GetU64(pl.data());
                    const uint8_t compressed = pl[8];
                    const uint32_t dataLen = GetU32(pl.data() + 9);
                    if (rawLen > kLiteralChunk || written + rawLen > newSize ||
                        13ull + dataLen != pl.size()) { failed = true; break; }
                    const uint8_t* src = pl.data() + 13;
                    if (compressed) {
                        litRaw.assign(rawLen, 0);
                        if (!Core::Deflate::DecompressExact(src, dataLen, litRaw.data(), rawLen,
                                                            Core::Deflate::Format::Raw)) { failed = true; break; }
                        writeChunks(litRaw.data(), rawLen);
                    } else {
                        if (dataLen != rawLen) { failed = true; break; }
                        writeChunks(src, rawLen);
                    }
                    written += rawLen; literalBytes += rawLen;
                } else { failed = true; break; }
            }
            out.flush(); out.close();
            base.Close();   // release the mapping before any rename (Windows)

            auto scrap = [&]() { error_code ig; fs::remove(incomingPath, ig); };

            if (failed || written != newSize) {
                Log::Error("[FileSync] transfer incomplete (%llu / %llu bytes); target untouched",
                           (unsigned long long)written, (unsigned long long)newSize);
                scrap(); return false;
            }
            if (hasher.Finish() != wholeHash) {
                Log::Error("[FileSync] VERIFY FAILED (hash mismatch); target untouched");
                scrap(); return false;
            }

            const double reusedPct = newSize ? (100.0 * copiedBytes / newSize) : 0.0;
            Log::Info("[FileSync] verified OK — %.1f MB, %.1f%% reused (copied %.1f MB, literal %.1f MB)",
                      newSize / 1048576.0, reusedPct, copiedBytes / 1048576.0, literalBytes / 1048576.0);

            if (dryRun) {
                Log::Info("[FileSync] dry run — not swapping; target left untouched");
                scrap();
                { std::vector<uint8_t> r{1}; SendFrame(io, sock, MsgType::Result, r); }
                return true;
            }

            error_code rec;
            if (fs::exists(target, rec)) {
                const std::string bak = targetPath + ".bak-" + Timestamp();
                fs::rename(target, bak, rec);
                if (rec) { Log::Error("[FileSync] could not back up existing target: %s", rec.message().c_str()); scrap(); return false; }
                Log::Info("[FileSync] existing target backed up to %s", bak.c_str());
            }
            fs::rename(fs::path(incomingPath), target, rec);
            if (rec) { Log::Error("[FileSync] final swap failed: %s", rec.message().c_str()); scrap(); return false; }
            Log::Info("[FileSync] updated %s", targetPath.c_str());
            { std::vector<uint8_t> r{1}; SendFrame(io, sock, MsgType::Result, r); }
            return true;
        }

        // ── Source: direct acceptor (UPnP-mapped port) ───────────────────────
        void RunDirectAcceptor(FileSyncService& svc, const std::string& servePath,
                               const std::shared_ptr<TicketGate>& gate) {
            net::io_context io;
            tcp::acceptor acc(io);
            error_code ec;
            acc.open(tcp::v4(), ec);
            if (ec) return;
            acc.set_option(tcp::acceptor::reuse_address(true), ec);
            acc.bind(tcp::endpoint(tcp::v4(), kFileSyncPort), ec);
            if (ec) { Log::Warning("[FileSync] direct acceptor bind failed: %s", ec.message().c_str()); return; }
            acc.listen(net::socket_base::max_listen_connections, ec);
            if (ec) return;
            Log::Info("[FileSync] direct acceptor listening on %u", (unsigned)kFileSyncPort);

            while (!svc.Stopping()) {
                tcp::socket sock(io);
                bool accepted = false;
                io.restart();
                acc.async_accept(sock, [&](const error_code& e) { accepted = !e; });
                io.run_for(std::chrono::seconds(2));
                if (!accepted) { error_code ig; acc.cancel(ig); io.run(); continue; }
                try { ServeDelta(io, sock, servePath, gate); }
                catch (...) { Log::Error("[FileSync] direct serve error"); }
            }
            error_code ig; acc.close(ig);
        }

        // ── Source: friends-service link (offer + relay-dial on request) ──────
        void RunSource(FileSyncService& svc, const FileSyncService::Params& p,
                       const std::string& servePath) {
            const std::string username = kFileSyncSourceUsername ? kFileSyncSourceUsername : "";
            auto gate = std::make_shared<TicketGate>();

            // UPnP map for the direct path (best effort; failure => relay only).
            Client::UPnPPortMapper upnp;
            auto mapres = upnp.Map(kFileSyncPort);
            std::string directHost = mapres.ok ? mapres.externalIp : "";
            uint16_t directPort = mapres.ok ? mapres.externalPort : 0;
            if (mapres.ok)
                Log::Info("[FileSync] UPnP mapped external %s:%u", directHost.c_str(), (unsigned)directPort);
            else
                Log::Info("[FileSync] UPnP unavailable (%s) — relay path only", mapres.error.c_str());

            // Direct acceptor on its own thread/io_context.
            std::thread acceptor;
            if (mapres.ok) {
                acceptor = std::thread([&svc, servePath, gate]() {
                    try { RunDirectAcceptor(svc, servePath, gate); }
                    catch (...) { Log::Error("[FileSync] direct acceptor crashed"); }
                });
            }

            svc.SetStatus("Serving chat.db (friends service)");

            // Service link with reconnect.
            while (!svc.Stopping()) {
                net::io_context io;
                tcp::socket sock(io);
                net::streambuf buf;
                if (!DialService(io, sock, p.serviceHost, p.servicePort, kConnectTimeout) ||
                    !ServiceHello(io, sock, buf, p.token)) {
                    Log::Warning("[FileSync] source: service connect/hello failed; retrying");
                    SleepWithStop(svc, std::chrono::seconds(5));
                    continue;
                }
                { json j{{"op", "filesync_offer"}, {"host", directHost}, {"port", directPort}, {"id", 2}};
                  if (!NdjsonWrite(io, sock, j.dump())) continue; }
                Log::Info("[FileSync] source: offering via friends service as '%s'", username.c_str());

                while (!svc.Stopping()) {
                    std::string line;
                    ReadRes r = NdjsonRead(io, sock, buf, line, kLinkPing);
                    if (r == ReadRes::Failed) break;                       // reconnect
                    if (r == ReadRes::TimedOut) {                          // keep-alive
                        json ping{{"op", "ping"}, {"id", 0}};
                        if (!NdjsonWrite(io, sock, ping.dump())) break;
                        continue;
                    }
                    // Got a line: act only on a file-sync relay request.
                    std::string ticket;
                    try {
                        json j = json::parse(line);
                        if (j.value("event", "") != "filesync_relay") continue;
                        ticket = j.value("ticket", "");
                    } catch (...) { continue; }
                    if (ticket.empty()) continue;

                    gate->Add(ticket);   // authorize this ticket for the relay serve
                    Log::Info("[FileSync] source: relay requested — dialing tunnel");
                    // Serve the relayed client on its own thread/io_context so
                    // the link keeps pinging during a large transfer.
                    std::thread([host = p.serviceHost, port = p.servicePort,
                                 ticket, servePath, gate]() {
                        try {
                            net::io_context rio;
                            tcp::socket rs(rio);
                            if (!DialService(rio, rs, host, port, kConnectTimeout)) return;
                            if (!RelayAttach(rio, rs, "host", ticket)) {
                                Log::Warning("[FileSync] source: relay attach failed"); return;
                            }
                            ServeDelta(rio, rs, servePath, gate);
                        } catch (...) { Log::Error("[FileSync] source: relay serve error"); }
                    }).detach();
                }
            }

            upnp.Unmap();
            // The acceptor polls Stopping() (now true) and returns on its own;
            // detach so shutdown is never held up by an in-flight serve.
            if (acceptor.joinable()) acceptor.detach();
        }

        // ── Client: find the source via the service and pull once ─────────────
        // Returns true when the migration is FINISHED (already done, or a
        // transfer verified OK this attempt) so the caller stops retrying;
        // false when nothing happened yet and it should try again later.
        bool RunClientOnce(FileSyncService& svc, const FileSyncService::Params& p,
                           const std::string& targetPath) {
            const std::string username = kFileSyncSourceUsername ? kFileSyncSourceUsername : "";
            const fs::path marker = fs::path(p.gameDir) / "file_sync_done.txt";
            error_code mec;
            if (fs::exists(marker, mec)) {
                Log::Info("[FileSync] chat.db already migrated (marker present); nothing to do");
                svc.SetStatus("chat.db already up to date");
                return true;
            }

            svc.SetStatus("Updating chat.db…");
            net::io_context io;
            tcp::socket sock(io);
            net::streambuf buf;
            if (!DialService(io, sock, p.serviceHost, p.servicePort, kConnectTimeout) ||
                !ServiceHello(io, sock, buf, p.token)) {
                Log::Warning("[FileSync] client: friends-service connect/hello failed; will retry");
                svc.SetStatus("chat.db update: service unavailable (will retry)");
                return false;
            }

            // Ask for the source until it is offering (or the window elapses).
            std::string ticket, directHost;
            uint16_t directPort = 0;
            bool got = false;
            int reqId = 10;
            const auto deadline = Clock::now() + kDiscoverWindow;
            while (!svc.Stopping() && Clock::now() < deadline && !got) {
                { json j{{"op", "filesync_connect"}, {"name", username}, {"id", reqId}};
                  if (!NdjsonWrite(io, sock, j.dump())) break; }
                const auto sub = Clock::now() + std::chrono::seconds(8);
                while (!svc.Stopping() && Clock::now() < sub) {
                    std::string line;
                    ReadRes r = NdjsonRead(io, sock, buf, line, std::chrono::seconds(8));
                    if (r == ReadRes::Failed) break;
                    if (r == ReadRes::TimedOut) break;
                    try {
                        json j = json::parse(line);
                        if (j.value("id", -1) != reqId) continue;          // skip roster/other pushes
                        if (j.value("ok", false)) {
                            ticket = j.value("ticket", "");
                            directHost = j.value("host", "");
                            directPort = (uint16_t)j.value("port", 0);
                            got = true;
                        } else {
                            Log::Info("[FileSync] client: source not ready (%s)",
                                      j.value("error", "?").c_str());
                        }
                    } catch (...) {}
                    break;   // got the reply to this request id
                }
                if (!got) { ++reqId; SleepWithStop(svc, std::chrono::seconds(3)); }
            }

            if (!got) {
                Log::Info("[FileSync] source '%s' not available yet via friends service; will retry",
                          username.c_str());
                svc.SetStatus("chat.db update: waiting for the other Mac…");
                return false;
            }

            bool ok = false;
            // Direct first (UPnP-mapped source port), if a usable address was given.
            if (!directHost.empty() && directPort != 0) {
                net::io_context dio;
                tcp::socket ds(dio);
                if (DialService(dio, ds, directHost, directPort, kDirectProbe)) {
                    Log::Info("[FileSync] client: direct path to %s:%u", directHost.c_str(), (unsigned)directPort);
                    try { ok = PullDelta(dio, svc, ds, targetPath, kFileSyncDryRun, ticket); }
                    catch (...) { ok = false; }
                }
            }
            // Relay fallback through the friends service.
            if (!ok && !svc.Stopping()) {
                net::io_context rio;
                tcp::socket rs(rio);
                if (DialService(rio, rs, p.serviceHost, p.servicePort, kConnectTimeout) &&
                    RelayAttach(rio, rs, "joiner", ticket)) {
                    Log::Info("[FileSync] client: relay path via friends service");
                    try { ok = PullDelta(rio, svc, rs, targetPath, kFileSyncDryRun, ticket); }
                    catch (...) { ok = false; }
                }
            }

            if (ok && !kFileSyncDryRun) {
                std::ofstream mf(marker.string(), std::ios::trunc);
                if (mf) mf << "chat.db migrated " << Timestamp() << "\n";
                svc.SetStatus("chat.db updated");
                return true;
            } else if (ok && kFileSyncDryRun) {
                svc.SetStatus("chat.db update: dry run verified OK");
                return true;   // a verified dry run is "done"; don't loop
            }
            svc.SetStatus("chat.db update failed (will retry)");
            return false;
        }
    } // namespace

    // ───────────────────────────────────────────────────────────────────────
    FileSyncService& FileSyncService::Instance() {
        static FileSyncService s;
        return s;
    }

    FileSyncService::~FileSyncService() {
        m_stop.store(true);
        if (m_thread.joinable()) m_thread.detach();
    }

    void FileSyncService::SetStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        m_status = s;
    }

    std::string FileSyncService::StatusLine() {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        return m_status;
    }

    void FileSyncService::StartIfConfigured(const Params& params) {
        const std::string username = kFileSyncSourceUsername ? kFileSyncSourceUsername : "";
        if (username.empty()) return;                       // feature OFF: literal no-op
        bool expected = false;
        if (!m_started.compare_exchange_strong(expected, true)) return;

        Params p = params;
        try {
            m_thread = std::thread([this, p]() {
                try { Worker(p); }
                catch (const std::exception& e) { Log::Error("[FileSync] worker exception: %s", e.what()); }
                catch (...) { Log::Error("[FileSync] worker unknown exception"); }
                m_finished.store(true);
            });
        } catch (...) {
            m_finished.store(true);
            Log::Error("[FileSync] failed to start worker thread");
        }
    }

    void FileSyncService::RequestStop() {
        m_stop.store(true);
        if (!m_thread.joinable()) return;
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!m_finished.load() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (m_finished.load() && m_thread.joinable()) m_thread.join();
        else if (m_thread.joinable())                 m_thread.detach();
    }

    void FileSyncService::Worker(Params params) {
        const std::string username = kFileSyncSourceUsername ? kFileSyncSourceUsername : "";
        const std::string servePath  = ExpandTilde(kServeFilePath);
        const std::string targetPath = ExpandTilde(kTargetFilePath);

        if (params.token.empty()) {
            Log::Warning("[FileSync] not signed into the friends service (guest) — "
                         "file sync needs the launcher login; will retry next launch");
            SetStatus("chat.db update: sign in from the launcher");
            return;
        }

        if (IEquals(params.localPlayerName, username)) {
            Log::Info("[FileSync] this Mac is the SOURCE (player '%s'); serving %s",
                      params.localPlayerName.c_str(), servePath.c_str());
            RunSource(*this, params, servePath);
        } else {
            Log::Info("[FileSync] this Mac is a CLIENT; reaching source '%s' via the friends service",
                      username.c_str());
            // Keep trying for the whole session (not just a startup window):
            // each attempt waits up to kDiscoverWindow for the other Mac to be
            // online and offering, so the transfer runs whenever you both
            // happen to be online — at the title screen or in a world. Stops
            // the moment it finishes (success/already-done) or the game quits.
            while (!Stopping()) {
                if (RunClientOnce(*this, params, targetPath)) break;
                SleepWithStop(*this, kClientRetryGap);
            }
        }
    }

} // namespace Sync
