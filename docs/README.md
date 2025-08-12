MyVoxelGame Architecture Documentation

This directory contains comprehensive architectural documentation for the MyVoxelGame voxel engine, organized as a professional documentation pack following C4 model principles.

Documentation Structure

📋 Overview (docs/overview/)

High-level system context and component relationships
- overview/00-context.md - Big picture, glossary, and key architectural decisions
- overview/10-containers.md - Client/Server/Network containers (C4 Level 2) with Mermaid diagrams
- overview/20-components.md - Major subsystems with responsibilities and file references

🧵 Threading & Queues (docs/threads-and-queues/)

Thread model and inter-thread communication
- threads-and-queues/thread-model.md - Exact responsibilities per thread, frame/tick order
- threads-and-queues/queues.md - All message queues, bounds, back-pressure handling

🌐 Protocol (docs/protocol/)

Network protocol specification and flows
- protocol/packet-registry.md - Complete packet table with IDs, states, listeners
- protocol/framing.md - Wire format, compression, VarInt encoding
- protocol/flows.md - Mermaid sequence diagrams for login, chunk streaming, block changes

🗺️ Chunks & Rendering (docs/chunks-and-render/)

World streaming and mesh pipeline
- chunks-and-render/client-chunk-manager.md - Data layout, API, dirty tracking
- chunks-and-render/meshing-pipeline.md - Worker handoff, GPU uploads, render layers
- chunks-and-render/chunk-lifecycle.md - Section state machine with Mermaid diagram

🖥️ Server (docs/server/)

Server architecture and 20 TPS tick loop
- server/player-session.md - Player state fields, per-tick processing steps
- server/chunk-watching.md - Watch sets vs watchers, tickets, send/remove logic
- server/tick-loop.md - Exact 20 TPS order, no io_context::poll() policy

📝 Architecture Decision Records (docs/adrs/)

Key architectural decisions with rationale
- adrs/0001-use-boost-asio.md - Why Boost.Asio (Netty equivalent)
- adrs/0002-typed-packets-and-listeners.md - Decode on I/O, apply on main
- adrs/0003-separate-io-threads.md - No polling in tick, dedicated I/O threads
- adrs/0004-packet-pipeline-flips-on-io-thread.md - Protocol state changes on I/O thread

📚 Runbooks (docs/runbooks/)

Step-by-step guides for development tasks
- runbooks/add-a-packet.md - Complete workflow for adding new packet types
- runbooks/add-a-thread.md - io_context + work guard pattern, shutdown checklist
- runbooks/add-a-render-layer.md - VBO/IBO layout, meshing changes, performance budgets

⚡ Performance (docs/perf/)

Performance targets and monitoring
- perf/budgets.md - Frame/tick budgets (ms), component limits, adaptive quality
- perf/logging-and-metrics.md - What to log/counter, OpenTelemetry integration

Key Implementation Files

Core Architecture

- src/server/IntegratedServer.cpp - Main server loop and 20 TPS tick processing
- src/client/world/ClientChunkManager.cpp - Client-side chunk state management
- src/server/network/ServerConnection.cpp - Per-connection packet handling and protocol states
- src/common/network/PacketRegistry.hpp - Packet ID definitions and VarInt encoding

Threading & Networking

- src/server/network/NetworkServer.cpp - TCP server with Boost.Asio I/O context
- src/server/network/ServerConnection.cpp - Per-connection I/O thread management
- src/common/network/MessageQueue.hpp - Thread-safe bounded queues
- src/client/renderer/mesh/ClientMeshManager.cpp - Mesh building coordination and GPU uploads

**I/O Thread Lifecycle**: Dedicated I/O threads run `io_context::run()` continuously with work guards, handling all network operations asynchronously without polling from main threads.

Ground-Truth Architectural Rules

This documentation reflects the authoritative architecture based on these core principles:

1. Mirror Minecraft Java Edition - Integrated server pattern with Boost.Asio (C++ Netty equivalent)
2. Dedicated Thread Separation - Client render (~60 FPS), server tick (20 TPS), I/O threads (continuous)
3. No Polling in Main Loops - I/O threads run io_context::run(), main threads never call poll()
4. Pipeline Flips on I/O Thread - Protocol/compression changes happen immediately on I/O thread
5. Bounded Queues Everywhere - All inter-thread communication uses bounded queues with back-pressure

Keeping Documentation Updated

When making significant architectural changes:

1. Update relevant documentation first - Architecture docs are the source of truth
2. Cross-reference code changes - Update file references and line numbers
3. Add/update Mermaid diagrams - Keep visual representations current
4. Update ADRs if needed - Document new architectural decisions
5. Test documentation accuracy - Verify examples and code snippets work

Documentation Dependencies

- Mermaid - For diagrams (render in GitHub, VS Code, etc.)
- Markdown - CommonMark specification compliance
- C4 Model - System context, containers, components hierarchy

External References

- https://wiki.vg/Protocol - Protocol compatibility reference
- https://www.boost.org/doc/libs/release/doc/html/boost_asio.html - Networking patterns
- https://c4model.com/ - Documentation structure methodology

**Note**: Singleplayer transport currently uses TCP localhost; vanilla Minecraft uses in-process LocalChannel.
