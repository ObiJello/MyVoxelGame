// File: src/client/dev/BotSwarm.cpp
#include "BotSwarm.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/network/ClientConnection.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/network/IPacketListener.hpp"
#include "common/network/IPacket.hpp"
#include "common/network/packets/common/PacketCommon.hpp"
#include "common/core/Log.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace Dev {

    namespace {

        using Clock = std::chrono::steady_clock;

        std::atomic<bool> g_stopRequested{false};
        void OnStopSignal(int) { g_stopRequested.store(true); }

        // Every "view around the destination loaded" measurement of the run,
        // for the end-of-run summary.
        struct Arrival { std::string dimension; double seconds; uint64_t chunks; };
        std::vector<Arrival> g_arrivals;
        bool g_warnedNoCommandAccess = false;
        int  g_teleportFailuresLogged = 0;

        // Scatter: how long until 50% / 90% of the view the server sends had
        // arrived after a teleport (a view full of terrain nobody had loaded),
        // and the destinations that never got there before the bot moved on.
        struct Fill { std::string dimension; double to50 = -1.0, to90 = -1.0; double fraction = 0.0; };
        std::vector<Fill> g_fills;

        // The chunks the server sends for a view: MC ChunkTrackingView.
        // isWithinDistance with the outer ring included (the server sends one
        // more ring than the client renders).
        uint64_t ExpectedViewChunks(int viewDistance) {
            uint64_t n = 0;
            const long long r2 = static_cast<long long>(viewDistance) * viewDistance;
            for (int dx = -viewDistance - 2; dx <= viewDistance + 2; ++dx) {
                for (int dz = -viewDistance - 2; dz <= viewDistance + 2; ++dz) {
                    const long long a = std::max(0, std::abs(dx) - 2);
                    const long long b = std::max(0, std::abs(dz) - 2);
                    if (a * a + b * b < r2) ++n;
                }
            }
            return n;
        }

        double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
        double Millis(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

        double Percentile(std::vector<double>& v, double p) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
            return v[i];
        }

        // The real client's estimator (ClientPacketHandler::ChunkBatchSizeCalculator:
        // same start, weight and clamp), timed on packet ARRIVAL instead of
        // apply — a bot applies nothing, and arrival is what the link decides.
        struct RateEstimator {
            double nanosPerChunk = 250000.0;
            int    weight = 1;
            Clock::time_point start{};

            void OnStart(Clock::time_point t) { start = t; }
            void OnFinished(int batchSize, Clock::time_point t) {
                if (batchSize <= 0 || start.time_since_epoch().count() == 0) return;
                const double sample = std::chrono::duration<double, std::nano>(t - start).count() / batchSize;
                const double clamped = std::clamp(sample, nanosPerChunk / 5.0, nanosPerChunk * 5.0);
                nanosPerChunk = (nanosPerChunk * weight + clamped) / (weight + 1);
                weight = std::min(9, weight + 1);
            }
            float Rate() const {
                return std::clamp(static_cast<float>(7000000.0 / nanosPerChunk), 0.01f, 256.0f);
            }
        };

        struct Bot;

        class BotListener final : public Network::IPacketListener {
        public:
            BotListener(Bot& bot, const BotSwarmOptions& options) : m_bot(bot), m_options(options) {}
            const char* getName() const override { return "BotListener"; }

            void onClientboundPlayerPosition(const Network::ClientboundPlayerPositionPacket& packet) override;
            void onChunkBatchStart() override;
            void onChunkBatchFinished(int batchSize) override;
            void onChangeDimensionS2C(const Network::ChangeDimensionS2CPacket& packet) override;
            void onChunkUnchangedS2C(const Network::ChunkUnchangedS2CPacket& packet) override;
            void onChatMessageS2C(const Network::ChatMessageS2CPacket& packet) override;
            void onDisconnect(const std::string& reason) override;

        private:
            Bot& m_bot;
            const BotSwarmOptions& m_options;
        };

        struct Bot {
            enum class State { Pending, Connecting, Online, Gone };

            int index = 0;
            std::string name;
            State state = State::Pending;
            std::unique_ptr<Client::NetworkClient> client;
            Client::ClientConnectionPtr conn;
            std::unique_ptr<BotListener> listener;
            Clock::time_point connectStartedAt{};
            Clock::time_point onlineAt{};
            std::string goneReason;

            // Protocol state.
            bool settingsSent = false;
            bool positioned = false;     // first ClientboundPlayerPosition applied
            bool loadedSent = false;
            uint32_t moveSeq = 0;
            int8_t dimension = Network::PlayerMoveC2SPacket::kDimensionUnknown;
            Clock::time_point packetTime{};   // arrival time of the packet being applied
            RateEstimator rate;

            // Movement.
            glm::dvec3 pos{0.0};
            glm::dvec3 home{0.0};
            double heading = 0.0;             // radians, 0 = +Z
            glm::dvec3 wanderTarget{0.0};
            bool arrived = false;
            Clock::time_point arrivedAt{};
            uint64_t chunksAtArrival = 0;
            bool settleReported = false;
            glm::dvec3 center{0.0};           // where it drifts once arrived
            std::string destination = "overworld";

            // Scatter: teleports by command.
            bool awaitingTeleport = false;
            bool fillOpen = false;            // a scatter destination being measured
            double to50 = -1.0, to90 = -1.0;
            Clock::time_point teleportRequestedAt{};
            Clock::time_point nextHopAt{};
            int hops = 0;
            std::mt19937 rng;

            // Stats.
            uint64_t chunksTotal = 0;
            uint64_t chunksWindow = 0;
            Clock::time_point lastBatchAt{};
            uint64_t bytesAtLastReport = 0;

            // Command probe (tick thread + reply path) and ping (network only).
            bool probeOutstanding = false;
            Clock::time_point probeSentAt{};
            Clock::time_point nextProbeAt{};
            Clock::time_point nextPingAt{};
            std::vector<double> probeSamplesMs;
            std::vector<double> pingSamplesMs;

            void Send(Network::PacketId id, const std::vector<uint8_t>& data) {
                if (conn && conn->IsConnected()) conn->SendPacket(static_cast<uint8_t>(id), data);
            }
        };

        void BotListener::onClientboundPlayerPosition(const Network::ClientboundPlayerPositionPacket& p) {
            using namespace Network::Relative;
            m_bot.pos.x = (p.relatives & X) ? m_bot.pos.x + p.x : p.x;
            m_bot.pos.y = (p.relatives & Y) ? m_bot.pos.y + p.y : p.y;
            m_bot.pos.z = (p.relatives & Z) ? m_bot.pos.z + p.z : p.z;
            if (!m_bot.positioned) {
                m_bot.home = m_bot.pos;
                m_bot.wanderTarget = m_bot.pos;
                m_bot.positioned = true;
            }
            if (m_bot.awaitingTeleport) {
                // The scatter teleport landed: the clock for "view loaded" starts.
                m_bot.awaitingTeleport = false;
                m_bot.arrived = true;
                m_bot.arrivedAt = m_bot.packetTime;
                m_bot.chunksAtArrival = m_bot.chunksTotal;
                m_bot.settleReported = false;
                m_bot.center = m_bot.pos;
                m_bot.fillOpen = true;
                m_bot.to50 = m_bot.to90 = -1.0;
            }
            // Exactly once per teleport: an ack with nothing pending is a kick.
            Network::ServerboundAcceptTeleportationPacket ack;
            ack.id = p.id;
            m_bot.Send(Network::PacketId::ServerboundAcceptTeleportation, Network::Serialization::Serialize(ack));
        }

        void BotListener::onChunkBatchStart() {
            m_bot.rate.OnStart(m_bot.packetTime);
        }

        void BotListener::onChunkBatchFinished(int batchSize) {
            m_bot.rate.OnFinished(batchSize, m_bot.packetTime);
            if (batchSize > 0) {
                m_bot.chunksTotal += static_cast<uint64_t>(batchSize);
                m_bot.chunksWindow += static_cast<uint64_t>(batchSize);
                m_bot.lastBatchAt = m_bot.packetTime;
            }
            const float rate = m_options.fixedRate > 0.0f ? m_options.fixedRate : m_bot.rate.Rate();
            m_bot.Send(Network::PacketId::ChunkBatchAckC2S,
                       Network::Serialization::Serialize(Network::ChunkBatchAckC2SPacket(rate)));
        }

        void BotListener::onChangeDimensionS2C(const Network::ChangeDimensionS2CPacket& p) {
            m_bot.dimension = p.dimensionId;
        }

        void BotListener::onChunkUnchangedS2C(const Network::ChunkUnchangedS2CPacket& p) {
            // A bot keeps nothing, so every "you still have this" is answered
            // the way a client that evicted it answers: send it again.
            Network::ChunkRequestFullC2SPacket req;
            req.chunkX = p.chunkX;
            req.chunkZ = p.chunkZ;
            req.dimensionId = m_bot.dimension == Network::PlayerMoveC2SPacket::kDimensionUnknown ? 0 : m_bot.dimension;
            m_bot.Send(Network::PacketId::ChunkRequestFullC2S, Network::Serialization::Serialize(req));
        }

        void BotListener::onChatMessageS2C(const Network::ChatMessageS2CPacket& p) {
            std::string text;
            for (const auto& seg : p.segments) text += seg.text;
            const bool commandError =
                text.find("not loaded") != std::string::npos || text.find("Unknown dimension") != std::string::npos ||
                text.find("No entity") != std::string::npos || text.find("Invalid position") != std::string::npos ||
                text.find("incomplete command") != std::string::npos || text.find("Error executing") != std::string::npos;
            if (m_bot.awaitingTeleport && commandError) {
                // The teleport command answered with something other than
                // success — show it, or a broken dimension hides for a whole run.
                m_bot.awaitingTeleport = false;
                if (g_teleportFailuresLogged < 5) {
                    ++g_teleportFailuresLogged;
                    Log::Warning("[Bots] %s: scatter teleport to %s failed: %s", m_bot.name.c_str(),
                                 m_bot.destination.c_str(), text.c_str());
                }
            }
            if (m_bot.awaitingTeleport && text.find("permission") != std::string::npos) {
                m_bot.awaitingTeleport = false;
                if (!g_warnedNoCommandAccess) {
                    g_warnedNoCommandAccess = true;
                    Log::Warning("[Bots] the server refused the scatter teleport — start it with "
                                 "--guest-commands (tools/stress_test.sh does this for scatter)");
                }
            }
            if (!m_bot.probeOutstanding) return;
            // "Unknown command: /stressprobe" with command access, the
            // permission refusal without — both come back from the tick thread.
            if (text.find("stressprobe") == std::string::npos && text.find("permission") == std::string::npos) return;
            m_bot.probeOutstanding = false;
            m_bot.probeSamplesMs.push_back(Millis(m_bot.packetTime - m_bot.probeSentAt));
        }

        void BotListener::onDisconnect(const std::string& reason) {
            m_bot.goneReason = reason;
        }

        // Golden-angle headings: any count of bots fans out evenly.
        double HeadingFor(int index, uint32_t seed) {
            const double runOffset = static_cast<double>(seed % 3600u) * (6.283185307179586 / 3600.0);
            return std::fmod(static_cast<double>(index) * 2.39996322972865332 + runOffset, 6.283185307179586);
        }

        void StepMovement(Bot& bot, const BotSwarmOptions& o, Clock::time_point now, std::mt19937& rng) {
            constexpr double kDt = 0.05;
            const double speed = std::clamp(o.speed, 0.0, 2000.0);   // under the 600-blocks-per-move check
            const glm::dvec3 dir(std::sin(bot.heading), 0.0, std::cos(bot.heading));
            const double cruiseY = std::min(bot.home.y + 20.0, 300.0);

            if (o.scenario == "fly") {
                bot.pos += dir * (speed * kDt);
                bot.pos.y = cruiseY;
            } else if (o.scenario == "spread") {
                const glm::dvec2 off(bot.pos.x - bot.home.x, bot.pos.z - bot.home.z);
                if (!bot.arrived && glm::length(off) < o.radius) {
                    bot.pos += dir * (speed * kDt);
                    bot.pos.y = cruiseY;
                } else {
                    if (!bot.arrived) {
                        bot.arrived = true;
                        bot.arrivedAt = now;
                        bot.chunksAtArrival = bot.chunksTotal;
                    }
                    // Drift a slow 16-block circle: present, but not exploring.
                    const double t = Seconds(now - bot.arrivedAt);
                    const glm::dvec3 centre = bot.home + dir * o.radius;
                    bot.pos = glm::dvec3(centre.x + 16.0 * std::cos(t * 0.06), cruiseY, centre.z + 16.0 * std::sin(t * 0.06));
                }
            } else if (o.scenario == "scatter") {
                if (bot.arrived && !bot.awaitingTeleport) {
                    const double t = Seconds(now - bot.arrivedAt);
                    bot.pos = glm::dvec3(bot.center.x + 16.0 * std::cos(t * 0.06), bot.center.y,
                                         bot.center.z + 16.0 * std::sin(t * 0.06));
                }
            } else if (o.scenario == "cluster") {
                const glm::dvec3 to(bot.wanderTarget.x - bot.pos.x, 0.0, bot.wanderTarget.z - bot.pos.z);
                const double dist = glm::length(to);
                constexpr double kWalk = 4.317;   // MC walking speed, blocks per second
                if (dist < 1.0) {
                    std::uniform_real_distribution<double> r(-48.0, 48.0);
                    bot.wanderTarget = glm::dvec3(bot.home.x + r(rng), bot.home.y, bot.home.z + r(rng));
                } else {
                    bot.pos += to / dist * std::min(dist, kWalk * kDt);
                    bot.heading = std::atan2(to.x, to.z);
                }
                bot.pos.y = bot.home.y;
            }
            // "idle": stay put.
        }

        void SendMove(Bot& bot) {
            // MC yaw 0 = +Z, increasing clockwise: -atan2(x, z).
            const float yaw = static_cast<float>(-bot.heading * 57.29577951308232);
            Network::PlayerMoveC2SPacket move(bot.pos, glm::vec2(yaw, 0.0f));
            move.onGround = true;            // no fall distance, no fall damage
            move.fallDistance = 0.0f;
            move.sequenceNumber = ++bot.moveSeq;
            move.dimensionId = Network::PlayerMoveC2SPacket::kDimensionUnknown;
            bot.Send(Network::PacketId::PlayerMoveC2S, Network::Serialization::Serialize(move));
        }

        void SendProbe(Bot& bot, Clock::time_point now) {
            Network::ChatMessageC2SPacket chat;
            chat.message = "/stressprobe";
            chat.timestamp = static_cast<uint32_t>(std::time(nullptr));
            chat.isCommand = true;
            bot.Send(Network::PacketId::ChatMessageC2S, Network::Serialization::Serialize(chat));
            bot.probeOutstanding = true;
            bot.probeSentAt = now;
        }

        std::vector<std::string> ParseDimensions(const std::string& list) {
            std::vector<std::string> out;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                std::string d = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                d.erase(0, d.find_first_not_of(' '));
                d.erase(d.find_last_not_of(' ') + 1);
                if (d == "the_nether" || d == "minecraft:the_nether") d = "nether";
                if (d == "the_end" || d == "minecraft:the_end") d = "end";
                if (d == "minecraft:overworld") d = "overworld";
                if (d == "overworld" || d == "nether" || d == "end") out.push_back(d);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            return out;
        }

        // Record the destination being left (or the run ending) as a fill sample.
        void CloseFill(Bot& bot, uint64_t expected) {
            if (!bot.fillOpen) return;
            bot.fillOpen = false;
            Fill f;
            f.dimension = bot.destination;
            f.to50 = bot.to50;
            f.to90 = bot.to90;
            f.fraction = expected ? static_cast<double>(bot.chunksTotal - bot.chunksAtArrival) / static_cast<double>(expected) : 0.0;
            g_fills.push_back(f);
        }

        // Scatter: a random spot in a random dimension, by command (vanilla's
        // `/execute in <dim> run tp x y z`, cross-dimension included).
        void RequestScatter(Bot& bot, const BotSwarmOptions& o, const std::vector<std::string>& dims,
                            Clock::time_point now) {
            std::uniform_real_distribution<double> coord(-o.scatterRange, o.scatterRange);
            std::uniform_int_distribution<size_t> pick(0, dims.size() - 1);
            const std::string& dim = dims[pick(bot.rng)];
            const long long x = static_cast<long long>(coord(bot.rng));
            const long long z = static_cast<long long>(coord(bot.rng));
            // Above the terrain in each: open sky, under the Nether roof, and
            // the End's island height (mostly void out there — as in vanilla).
            const int y = dim == "nether" ? 70 : (dim == "end" ? 80 : 140);
            const char* id = dim == "nether" ? "minecraft:the_nether"
                           : (dim == "end" ? "minecraft:the_end" : "minecraft:overworld");
            CloseFill(bot, ExpectedViewChunks(o.viewDistance));
            char cmd[160];
            std::snprintf(cmd, sizeof(cmd), "/execute in %s run tp %lld %d %lld", id, x, y, z);

            Network::ChatMessageC2SPacket chat;
            chat.message = cmd;
            chat.timestamp = static_cast<uint32_t>(std::time(nullptr));
            chat.isCommand = true;
            bot.Send(Network::PacketId::ChatMessageC2S, Network::Serialization::Serialize(chat));
            bot.awaitingTeleport = true;
            bot.teleportRequestedAt = now;
            bot.destination = dim;
            bot.arrived = false;
            ++bot.hops;
        }

        // Undecoded packets a bot still acts on (should they ever arrive raw).
        void ApplyRaw(Bot& bot, const Network::RawPayloadPacket& raw) {
            if (raw.rawId() == static_cast<uint8_t>(Network::PacketId::ChunkBatchStartS2C)) {
                bot.listener->onChunkBatchStart();
            } else if (raw.rawId() == static_cast<uint8_t>(Network::PacketId::ChunkBatchFinishedS2C) &&
                       raw.payload().size() >= 4) {
                Network::PacketReader r(raw.payload());
                bot.listener->onChunkBatchFinished(static_cast<int>(r.ReadInt()));
            }
        }

    } // namespace

    int RunBotSwarm(const BotSwarmOptions& o) {
        g_stopRequested.store(false);
        std::signal(SIGINT, OnStopSignal);
        std::signal(SIGTERM, OnStopSignal);

        const int count = std::clamp(o.count, 1, 1000);
        Log::Info("[Bots] %d bots -> %s:%u, scenario '%s', join every %.2fs, speed %.0f b/s, radius %.0f, "
                  "view %d / sim %d, rate %s, probe every %.1fs",
                  count, o.host.c_str(), static_cast<unsigned>(o.port), o.scenario.c_str(), o.joinIntervalSec,
                  o.speed, o.radius, o.viewDistance, o.simulationDistance,
                  o.fixedRate > 0.0f ? "fixed" : "measured", o.probeIntervalSec);
        if (o.scenario != "spread" && o.scenario != "fly" && o.scenario != "cluster" &&
            o.scenario != "scatter" && o.scenario != "idle") {
            Log::Error("[Bots] unknown --bot-scenario '%s' (spread | fly | cluster | scatter | idle)", o.scenario.c_str());
            return 2;
        }
        const uint32_t seed = o.seed != 0 ? o.seed
            : static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count() & 0x7fffffff) | 1u;
        const std::vector<std::string> dims = ParseDimensions(o.dimensions);
        if (o.scenario == "scatter") {
            if (dims.empty()) {
                Log::Error("[Bots] --bot-dimensions '%s' names no dimension (overworld, nether, end)", o.dimensions.c_str());
                return 2;
            }
            Log::Info("[Bots] scatter: +/-%.0f blocks over %s, %s", o.scatterRange, o.dimensions.c_str(),
                      o.hopIntervalSec > 0.0 ? ("hopping every " + std::to_string(static_cast<int>(o.hopIntervalSec)) + " s").c_str()
                                             : "once");
        }
        Log::Info("[Bots] run seed %u (repeat this run with --bot-seed %u)", seed, seed);
        g_arrivals.clear();
        g_fills.clear();
        g_warnedNoCommandAccess = false;
        g_teleportFailuresLogged = 0;
        const uint64_t expectedView = ExpectedViewChunks(o.viewDistance);

        // One io_context for every connection; each connection has its own
        // strand, so a few threads serve them all.
        net::io_context io;
        auto work = net::make_work_guard(io);
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned ioThreads = std::clamp(hw / 2, 1u, 4u);
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < ioThreads; ++i) threads.emplace_back([&io] { io.run(); });

        std::vector<std::unique_ptr<Bot>> bots;
        bots.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            auto bot = std::make_unique<Bot>();
            bot->index = i;
            bot->name = o.namePrefix + std::to_string(i + 1);
            bot->heading = HeadingFor(i, seed);
            bot->rng.seed(seed ^ (0x9E3779B9u * static_cast<uint32_t>(i + 1)));
            bot->listener = std::make_unique<BotListener>(*bot, o);
            bots.push_back(std::move(bot));
        }

        std::FILE* csv = nullptr;
        if (!o.csvPath.empty()) {
            csv = std::fopen(o.csvPath.c_str(), "w");
            if (csv) {
                std::fprintf(csv, "second,online,pending,gone,chunks_per_s,chunks_per_bot_avg,chunks_per_bot_min,"
                                  "mb_in_per_s,cmd_rtt_p50_ms,cmd_rtt_p95_ms,cmd_rtt_max_ms,cmd_stuck,"
                                  "ping_p50_ms,ping_p95_ms,ping_max_ms\n");
                Log::Info("[Bots] writing %s", o.csvPath.c_str());
            } else {
                Log::Warning("[Bots] cannot write %s", o.csvPath.c_str());
            }
        }

        std::mt19937 rng(12345u);
        const Clock::time_point start = Clock::now();
        Clock::time_point nextTick = start;
        Clock::time_point nextReport = start + std::chrono::seconds(1);
        int nextJoin = 0;
        int64_t second = 0;
        int disconnects = 0;

        while (!g_stopRequested.load()) {
            const Clock::time_point now = Clock::now();
            if (o.quitAfterSec > 0.0 && Seconds(now - start) >= o.quitAfterSec) break;

            // ── Joins ────────────────────────────────────────────────────
            while (nextJoin < count &&
                   (o.joinIntervalSec <= 0.0 || Seconds(now - start) >= o.joinIntervalSec * nextJoin)) {
                Bot& bot = *bots[static_cast<size_t>(nextJoin++)];
                bot.client = std::make_unique<Client::NetworkClient>(io);
                bot.client->SetPlayerName(bot.name);
                bot.client->SetPlayerColor(static_cast<uint8_t>(bot.index % 16));
                bot.client->SetLightweightDecode(true);
                bot.client->ConnectAsync(o.host, o.port);
                bot.state = Bot::State::Connecting;
                bot.connectStartedAt = now;
                // Stagger the probes so they do not all land on one tick.
                bot.nextProbeAt = now + std::chrono::milliseconds(
                    static_cast<int64_t>(o.probeIntervalSec * 1000.0) + (bot.index * 97) % 1000);
                bot.nextPingAt = now + std::chrono::milliseconds(2000 + (bot.index * 53) % 1000);
            }

            // ── Bots ─────────────────────────────────────────────────────
            for (auto& botPtr : bots) {
                Bot& bot = *botPtr;
                if (bot.state == Bot::State::Connecting) {
                    if (bot.client->IsConnected()) {
                        auto conn = bot.client->GetConnection();
                        if (conn && conn->IsLoggedIn()) {
                            bot.conn = std::move(conn);
                            bot.state = Bot::State::Online;
                            bot.onlineAt = now;
                        }
                    }
                    if (bot.state == Bot::State::Connecting && Seconds(now - bot.connectStartedAt) > 20.0) {
                        bot.state = Bot::State::Gone;
                        bot.goneReason = "no login within 20 s";
                        Log::Warning("[Bots] %s: %s", bot.name.c_str(), bot.goneReason.c_str());
                        ++disconnects;
                    }
                    continue;
                }
                if (bot.state != Bot::State::Online) continue;
                if (!bot.conn || !bot.conn->IsConnected()) {
                    bot.state = Bot::State::Gone;
                    if (bot.goneReason.empty()) bot.goneReason = "connection closed";
                    Log::Warning("[Bots] %s disconnected after %.0fs: %s", bot.name.c_str(),
                                 Seconds(now - bot.onlineAt), bot.goneReason.c_str());
                    ++disconnects;
                    continue;
                }

                if (!bot.settingsSent) {
                    bot.conn->SendClientSettings(o.viewDistance, o.simulationDistance, false, 1.0f);
                    bot.settingsSent = true;
                }

                // Drain everything that arrived. Chunk data was never decoded
                // (lightweight connection), so this is cheap.
                Network::IncomingPacket in;
                int budget = 100000;
                while (budget-- > 0 && bot.conn->TryPopIncoming(in)) {
                    if (!in.packet) continue;
                    bot.packetTime = in.timestamp;
                    if (auto* s2c = dynamic_cast<Network::IS2CPacket*>(in.packet.get())) {
                        s2c->apply(*bot.listener);
                    } else if (auto* raw = dynamic_cast<Network::RawPayloadPacket*>(in.packet.get())) {
                        ApplyRaw(bot, *raw);
                    }
                }

                if (!bot.positioned) continue;
                if (!bot.loadedSent) {
                    // MC ServerboundPlayerLoadedPacket: moves are accepted from here.
                    bot.Send(Network::PacketId::PlayerLoadedC2S, {});
                    bot.loadedSent = true;
                }
                StepMovement(bot, o, now, rng);
                SendMove(bot);

                // Spread: the view around the destination is loaded once the
                // chunk stream has been quiet for two seconds.
                if (bot.fillOpen) {
                    const uint64_t got = bot.chunksTotal - bot.chunksAtArrival;
                    if (bot.to50 < 0.0 && got * 2 >= expectedView) bot.to50 = Seconds(now - bot.arrivedAt);
                    if (bot.to90 < 0.0 && got * 10 >= expectedView * 9) {
                        bot.to90 = Seconds(now - bot.arrivedAt);
                        Log::Info("[Bots] %s: 90%% of the view at its destination (%s) arrived %.1fs after the teleport "
                                  "(50%% at %.1fs)", bot.name.c_str(), bot.destination.c_str(), bot.to90, bot.to50);
                    }
                }
                if (o.scenario != "scatter" && bot.arrived && !bot.settleReported && bot.lastBatchAt > bot.arrivedAt &&
                    Seconds(now - bot.lastBatchAt) >= 2.0) {
                    bot.settleReported = true;
                    const double secs = Seconds(bot.lastBatchAt - bot.arrivedAt);
                    const uint64_t got = bot.chunksTotal - bot.chunksAtArrival;
                    g_arrivals.push_back({bot.destination, secs, got});
                    Log::Info("[Bots] %s: view around its destination (%s) loaded %.1fs after arriving (%llu chunks)",
                              bot.name.c_str(), bot.destination.c_str(), secs, static_cast<unsigned long long>(got));
                }

                if (o.scenario == "scatter") {
                    const bool firstJump = bot.hops == 0;
                    const bool hopDue = o.hopIntervalSec > 0.0 && bot.hops > 0 && now >= bot.nextHopAt;
                    if (!bot.awaitingTeleport && (firstJump || hopDue)) {
                        RequestScatter(bot, o, dims, now);
                        bot.nextHopAt = now + std::chrono::milliseconds(static_cast<int64_t>(o.hopIntervalSec * 1000.0));
                    } else if (bot.awaitingTeleport && Seconds(now - bot.teleportRequestedAt) > 30.0) {
                        Log::Warning("[Bots] %s: scatter teleport unanswered for 30 s", bot.name.c_str());
                        bot.awaitingTeleport = false;
                    }
                }

                if (o.probeIntervalSec > 0.0 && !bot.probeOutstanding && now >= bot.nextProbeAt) {
                    SendProbe(bot, now);
                    bot.nextProbeAt = now + std::chrono::milliseconds(static_cast<int64_t>(o.probeIntervalSec * 1000.0));
                }
                const int32_t ping = bot.conn->ConsumePingRtt();
                if (ping >= 0) bot.pingSamplesMs.push_back(static_cast<double>(ping));
                if (now >= bot.nextPingAt) {
                    bot.conn->SendPingRequest();
                    bot.nextPingAt = now + std::chrono::seconds(2);
                }
            }

            // ── Report ───────────────────────────────────────────────────
            if (now >= nextReport) {
                const double window = 1.0 + Seconds(now - nextReport);
                int online = 0, pending = 0, gone = 0, stuck = 0;
                uint64_t chunks = 0, minChunks = UINT64_MAX, bytesIn = 0;
                std::vector<double> probes, pings;
                for (auto& botPtr : bots) {
                    Bot& bot = *botPtr;
                    switch (bot.state) {
                        case Bot::State::Online: ++online; break;
                        case Bot::State::Gone:   ++gone;   break;
                        default:                 ++pending; break;
                    }
                    if (bot.state == Bot::State::Online) {
                        chunks += bot.chunksWindow;
                        minChunks = std::min(minChunks, bot.chunksWindow);
                        if (bot.conn) {
                            const uint64_t b = bot.conn->GetStats().bytesReceived.load(std::memory_order_relaxed);
                            bytesIn += b >= bot.bytesAtLastReport ? b - bot.bytesAtLastReport : 0;
                            bot.bytesAtLastReport = b;
                        }
                        // A probe unanswered for 5 s is the symptom itself.
                        if (bot.probeOutstanding && Seconds(now - bot.probeSentAt) > 5.0) ++stuck;
                    }
                    bot.chunksWindow = 0;
                    probes.insert(probes.end(), bot.probeSamplesMs.begin(), bot.probeSamplesMs.end());
                    pings.insert(pings.end(), bot.pingSamplesMs.begin(), bot.pingSamplesMs.end());
                    bot.probeSamplesMs.clear();
                    bot.pingSamplesMs.clear();
                }
                if (online == 0) minChunks = 0;
                const double perBot = online ? static_cast<double>(chunks) / online / window : 0.0;
                const double cmdP50 = Percentile(probes, 0.50), cmdP95 = Percentile(probes, 0.95);
                const double cmdMax = probes.empty() ? 0.0 : probes.back();
                const double pingP50 = Percentile(pings, 0.50), pingP95 = Percentile(pings, 0.95);
                const double pingMax = pings.empty() ? 0.0 : pings.back();
                const double mbIn = static_cast<double>(bytesIn) / (1024.0 * 1024.0) / window;
                Log::Info("[Bots] t=%llds online=%d pending=%d gone=%d | chunks/s total=%.0f per-bot avg=%.1f min=%.0f | "
                          "in=%.2fMB/s | cmdRTT p50=%.0f p95=%.0f max=%.0fms stuck=%d | ping p50=%.0f p95=%.0f max=%.0fms",
                          static_cast<long long>(second), online, pending, gone,
                          static_cast<double>(chunks) / window, perBot, static_cast<double>(minChunks) / window,
                          mbIn, cmdP50, cmdP95, cmdMax, stuck, pingP50, pingP95, pingMax);
                if (csv) {
                    std::fprintf(csv, "%lld,%d,%d,%d,%.1f,%.2f,%.1f,%.3f,%.1f,%.1f,%.1f,%d,%.1f,%.1f,%.1f\n",
                                 static_cast<long long>(second), online, pending, gone,
                                 static_cast<double>(chunks) / window, perBot, static_cast<double>(minChunks) / window,
                                 mbIn, cmdP50, cmdP95, cmdMax, stuck, pingP50, pingP95, pingMax);
                    std::fflush(csv);
                }
                ++second;
                nextReport = now + std::chrono::seconds(1);

                if (nextJoin >= count && gone == count) {
                    Log::Warning("[Bots] every bot is gone — stopping");
                    break;
                }
            }

            nextTick += std::chrono::milliseconds(50);
            const Clock::time_point after = Clock::now();
            if (nextTick < after) nextTick = after;   // behind: no catch-up burst
            std::this_thread::sleep_until(nextTick);
        }

        Log::Info("[Bots] stopping (%d disconnects during the run)", disconnects);
        for (auto& bot : bots) CloseFill(*bot, expectedView);
        if (!g_fills.empty()) {
            Log::Info("[Bots] scatter fill: a view is %llu chunks at view distance %d",
                      static_cast<unsigned long long>(expectedView), o.viewDistance);
            for (const char* dim : {"overworld", "nether", "end"}) {
                std::vector<double> t50, t90, frac;
                size_t n = 0;
                for (const Fill& f : g_fills) {
                    if (f.dimension != dim) continue;
                    ++n;
                    if (f.to50 >= 0.0) t50.push_back(f.to50);
                    if (f.to90 >= 0.0) t90.push_back(f.to90);
                    frac.push_back(std::min(1.0, f.fraction));
                }
                if (n == 0) continue;
                double fracAvg = 0.0;
                for (double v : frac) fracAvg += v;
                fracAvg /= static_cast<double>(frac.size());
                const double p50of50 = Percentile(t50, 0.5);
                const double p50of90 = Percentile(t90, 0.5), p95of90 = Percentile(t90, 0.95);
                Log::Info("[Bots] summary %s: %zu destinations | 50%% of view in p50=%.1fs (%zu reached) | "
                          "90%% in p50=%.1fs p95=%.1fs max=%.1fs (%zu reached) | %zu never reached 90%%, "
                          "avg %.0f%% of the view had arrived when the bot left",
                          dim, n, p50of50, t50.size(), p50of90, p95of90, t90.empty() ? 0.0 : t90.back(),
                          t90.size(), n - t90.size(), fracAvg * 100.0);
            }
        }
        // How long a fresh area took to load, per dimension (spread/scatter).
        if (!g_arrivals.empty()) {
            for (const char* dim : {"overworld", "nether", "end"}) {
                std::vector<double> secs;
                uint64_t chunks = 0;
                for (const Arrival& a : g_arrivals) {
                    if (a.dimension != dim) continue;
                    secs.push_back(a.seconds);
                    chunks += a.chunks;
                }
                if (secs.empty()) continue;
                const size_t n = secs.size();
                const double p50 = Percentile(secs, 0.50), p95 = Percentile(secs, 0.95);
                Log::Info("[Bots] summary %s: %zu arrivals, view loaded in p50=%.1fs p95=%.1fs max=%.1fs, "
                          "%.0f chunks each on average",
                          dim, n, p50, p95, secs.back(), static_cast<double>(chunks) / static_cast<double>(n));
            }
        }
        for (auto& bot : bots) {
            if (bot->client) bot->client->Disconnect();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        work.reset();
        io.stop();
        for (auto& t : threads) t.join();
        for (auto& bot : bots) {
            bot->conn.reset();
            bot->client.reset();
        }
        if (csv) std::fclose(csv);
        return 0;
    }

} // namespace Dev
