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
       ┌────────────┐
       │   Render   │
       │ (Shader,   │
       │  Quad,     │
       │  Mesh,     │
       │  Renderer) │
       └─────┬──────┘
             │
             ▼
       ┌────────────┐
       │    Game    │
       │ (World,    │
       │  Chunk,    │
       │  BlockReg, │
       │  Server)   │
       └────────────┘
