// File: src/client/dev/BotSwarm.hpp
//
// Headless bot swarm — the client half of the multiplayer stress test.
//
//   MyVoxelGame --bots 50 --server 192.168.1.20:25565 [--bot-scenario spread]
//
// N fake players in one process, no window: each one logs in with the real
// client connection code (handshake, compression, keep-alive), acknowledges
// chunk batches the way a real client does (MC's ChunkBatchSizeCalculator,
// timed on packet ARRIVAL, so the requested rate follows the link), acks
// teleports, reports itself loaded, and moves at 20 Hz by a scenario:
//
//   spread   fly out along its own heading to --bot-radius, then drift —
//            N separate areas generated and held at once (default)
//   fly      fly outward forever at --bot-speed: constant new terrain
//   cluster  wander within 48 blocks of spawn: everyone sees everyone
//   scatter  teleport to a random spot within --bot-range in a random
//            dimension (--bot-dimensions), wait there, and with --bot-hop
//            jump to a new one every N seconds — generation and sending of
//            terrain nobody has loaded, spread over every dimension. Needs
//            command access (the headless server's --guest-commands)
//   idle     stand at spawn
//
// Headings and scatter spots come from --bot-seed (default: a new seed each
// run, logged), so repeated runs in one world still meet fresh terrain.
//
// Chunk data is never parsed (ClientConnection::SetLightweightDecode), so the
// bots cost the machine running them little and the numbers describe the
// server, not the swarm.
//
// Measured per bot and reported once a second ("[Bots]" lines + CSV under
// <obeycraft>/logs/bots/):
//   - chunks received per second;
//   - command round trip: a "/stressprobe" command every --bot-probe seconds
//     goes the way a player's command goes (tick thread, then the reply back
//     through the same queue as the chunks) — the "my commands stop going
//     through" number;
//   - ping (I/O-thread echo: the network alone, without the tick);
//   - for spread: time from arriving at the destination until its chunk
//     stream went quiet (view loaded).
#pragma once

#include <cstdint>
#include <string>

namespace Dev {

    struct BotSwarmOptions {
        int         count = 10;
        std::string host = "127.0.0.1";
        uint16_t    port = 25565;
        std::string scenario = "spread";     // spread | fly | cluster | scatter | idle
        uint32_t    seed = 0;                // 0 = new each run
        double      scatterRange = 20000.0;  // scatter: +/- blocks on x and z
        double      hopIntervalSec = 0.0;    // scatter: re-scatter every N s (0 = once)
        std::string dimensions = "overworld,nether,end";   // scatter: where to go
        double      joinIntervalSec = 0.25;  // 0 = all at once (join storm)
        double      speed = 20.0;            // blocks per second (spread / fly)
        double      radius = 1500.0;         // spread: distance from spawn
        int         viewDistance = 12;
        int         simulationDistance = 8;
        float       fixedRate = 0.0f;        // > 0: ack with this chunks/tick instead of measuring
        double      probeIntervalSec = 5.0;
        double      quitAfterSec = 0.0;      // 0 = until Ctrl+C
        std::string namePrefix = "Bot";
        std::string csvPath;                 // empty = log lines only
    };

    // Runs until every bot is gone, --quit-after, or SIGINT/SIGTERM. Returns
    // the process exit code.
    int RunBotSwarm(const BotSwarmOptions& options);

} // namespace Dev
