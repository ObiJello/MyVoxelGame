# MyVoxelGame Architecture Overview

This document provides a high-level diagram and explanation of the main subsystems in MyVoxelGame. Each layer is isolated behind clear interfaces so that higher‐level code never directly depends on platform APIs or low‐level details.

       ┌────────────┐
       │  Platform  │
       │  (GLFW /   │
       │   GLAD /   │
       │   Time /   │
       │   Input)   │
       └─────┬──────┘
             │
             ▼
       ┌────────────┐
       │    Core    │
       │ (JobSystem,│
       │   Log,     │
       │   Config)  │
       └─────┬──────┘
             │
             ▼
       ┌────────────┐        ┌────────────┐
       │   Render   │        │  Network   │
       │ (Shader,   │        │ (Boost.    │
       │  Quad,     │        │  Asio,     │
       │  Mesh,     │        │  I/O       │
       │  Renderer) │        │  Threads)  │
       └─────┬──────┘        └─────┬──────┘
             │                     │
             ▼                     ▼
       ┌─────────────────────────────────┐
       │            Game                 │
       │        (World, Chunk,           │
       │       BlockReg, Server)         │
       └─────────────────────────────────┘

## Network Layer

The Network layer handles all I/O operations through Boost.Asio:
- **NetworkServer**: TCP server binding and connection acceptance
- **ServerConnection**: Per-connection packet handling with protocol state management
- **I/O Threads**: Run `io_context::run()` continuously with work guards
- **Message Queues**: Bounded queues for thread-safe packet passing

For detailed networking patterns, see docs/threads-and-queues/ and docs/protocol/.
