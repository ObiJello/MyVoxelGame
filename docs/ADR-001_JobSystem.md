## 1. Platform Layer

- **Purpose**: Isolate all OS‐ and third‐party‐library‐specific code (GLFW, GLAD, `<chrono>`, etc.) so the rest of the engine never directly includes those headers.
- **Key Components**:
  - `PlatformMain`: Creates the GLFW window, initializes GLAD, sets up debug callbacks.
  - `Time`: Wraps `std::chrono::steady_clock` to expose `Time::Tick()` and `Time::Delta()` for frame‐timing.
  - `Input`: Wraps GLFW key/mouse/scroll functions behind an `Input::IsKeyDown(Key)` API.
  - (In the future) server‐socket initialization for dedicated server builds.

## 2. Core Layer

- **Purpose**: Provide cross‐platform, engine‐level utilities that do not depend on OS or rendering details.
- **Key Components**:
  - **JobSystem**: A simple thread pool (`ThreadPool`) for asynchronous tasks (e.g., chunk mesh generation). Exposes a global `g_ThreadPool`.
  - **Log**: A small `printf`‐style logger with colored output (DEBUG in blue, INFO in green, ERROR in red).  
  - **Config**: Compile‐time constants (window size, OpenGL version, chunk dimensions, build‐limits).
- **Interactions**:
  - Called by both `PlatformMain` (to log and read config) and higher‐level systems (e.g., world generation tasks).

## 3. Render Layer

- **Purpose**: Encapsulate all OpenGL logic (shader compilation, mesh buffering, rendering). Game code only calls into this layer’s classes.
- **Key Components**:
  - **Shader**: Loads GLSL files from `shaders/`, compiles, links, and supports hot‐reload in Debug when the file timestamps change. Caches uniform locations.
  - **QuadRenderer**: Creates a simple full‐screen quad (two triangles) with colored vertices to verify the rendering pipeline. Calls `Shader::HotReloadIfNeeded()` each frame.
  - (Future) `Mesh`, `TextureAtlas`, `Renderer` classes for batched block‐mesh rendering, frustum culling, etc.
- **Interactions**:
  - `PlatformMain` constructs a `QuadRenderer` and calls `Draw()` each frame.
  - Later, `Game` code will call into `Renderer` to draw chunks and entities.

## 4. Game Layer

- **Purpose**: Contain all voxel‐world logic: block definitions, chunk storage, world generation, integrated server tick, entity management.
- **Key Components**:
  - **Blocks & BlockRegistry**:  
    - `enum class BlockID { Air = 0, Stone = 1, Dirt = 2, Grass = 3, Count }`.  
    - `Block` struct (name, opacity, per‐face texture index).  
    - `BlockRegistry::Init()` registers each `BlockID`‐→`Block` definition in a static array.  
  - **World & Chunk** (to be implemented in Phase 2):  
    - A `Chunk` holds a 16×16×256 array of `BlockID` (2 bytes per voxel).  
    - Internally subdivided into 16×16×16 “sub‐chunks” for mesh building.  
    - Mesh building jobs enqueued on `JobSystem::g_ThreadPool`.  
  - **Server Stub / Integrated Server** (to be implemented in Phase 4):  
    - Single‐player spins up an internal server thread running at 20 TPS, communicating via loopback packets.  
    - Dedicated headless server build spawns the same code without rendering.
- **Interactions**:
  - On startup: `PlatformMain` calls `BlockRegistry::Init()`.  
  - During a “tick”: world generation jobs use `JobSystem` and `Renderer` to produce GPU meshes.  
  - Input from `Platform` drives player movement; world code applies physics and entity updates; render code draws the result.

---

## How Data Flows Between Layers

1. **Startup**:  
   - `main()` (or `PlatformMain::Run`) calls:  
     - `Log::Init()`  
     - `Game::BlockRegistry::Init()`  
     - `glfwInit()` → create window → `gladLoadGLLoader()`  
   - After that, layers are “live.”

2. **Per‐Frame (PlatformMain Loop)**:  
   1. `Time::Tick()` → compute frame delta.  
   2. `Input::IsKeyDown(...)` → handle user input.  
   3. `QuadRenderer.Draw()` (in Phase 1) or eventually `Renderer.DrawFrame(world)` in Phase 2+.  
   4. `glfwPollEvents()`, `glfwSwapBuffers()`.  
   5. Under the hood, a `JobSystem` worker may have finished generating chunk meshes, and `Renderer` uploads vertex buffers accordingly.

3. **Asynchronous Job System**:  
   - When the game requests a new chunk mesh (e.g., scrolling into a new area), it calls:  
     ```cpp
     JobSystem::g_ThreadPool.Enqueue([=] {
         MeshData mesh = BuildChunkMesh(chunkX, chunkZ);
         // then queue a main‐thread task to upload to GPU
     });
     ```
   - Worker threads run `BuildChunkMesh(...)`; once ready, they pass the mesh to the Render layer.

4. **Block Registry Lookups**:  
   - When generating chunk data, the world code does:  
     ```cpp
     BlockID id = chunk.getBlock(x,y,z);
     const auto& def = Game::BlockRegistry::Get(id);
     if (def.opaque) { /* render face */ }
     uint8_t texIndex = def.texIdx[face];
     ```
   - All per‐voxel storage is just `BlockID`, so memory remains compact.

---

## Next Steps

- **Phase 2** (World Generation & Chunk Pipeline):  
  - Wrap a noise library (FastNoiseLite) → Terrain generator → Structure hooks.  
  - Build `Chunk` data structures, sub‐chunk mesh jobs, asynchronous GPU upload.  
- **Phase 3** (Rendering Features):  
  - Greedy meshing, frustum culling, skybox, water, texture atlas.  
- **Phase 4** (Gameplay & Integrated Server):  
  - Tick loop, block‐update graph (redstone, fluids), entity system, inventory UI, audio.  
- **Phase 5** (Persistence, Tools & Polish):  
  - Custom region files, conversion tools, resource packs, profiler overlay, mod hooks.

---

---

**File: `docs/ADR-001_JobSystem.md`**

```markdown
# ADR-001: Job System Design

## Status
Proposed

## Context
We need a mechanism to perform time‐consuming tasks (e.g., chunk mesh generation, world generation) off the main thread so the renderer stays responsive at 60 FPS. Java‐Minecraft uses a built‐in thread pool to asynchronously build chunk meshes. In C++, we must choose a design that is:

- **Simple to implement & understand**  
- **Efficient under contention** (many chunk‐mesh jobs may be queued concurrently)  
- **Portable to Windows, macOS, Linux**

## Decision
Implement a basic, blocking `ThreadPool` class in `src/core/JobSystem.hpp/.cpp`:

- **ThreadPool** spawns `N` worker threads at construction (where `N = hardware_concurrency() - 1`, but at least 1 thread).
- A `std::queue<std::function<void()>>` holds pending jobs.
- A `std::mutex` + `std::condition_variable` coordinate access between enqueueing (producer) and workers (consumers).
- `Enqueue(job)` locks the queue, pushes the `std::function`, then notifies one worker.
- Each worker thread loops, waiting for either a new job or a shutdown signal (`stop = true`). When signaled, the worker pops a job under lock, unlocks, runs the job, then repeats. On shutdown, or if `stop` is set and no jobs remain, the worker exits.
- A global instance `g_ThreadPool` is defined in `JobSystem.cpp`, so game code can simply call `JobSystem::g_ThreadPool.Enqueue(...)` without passing around references.

### Why Not Lock-Free or Coroutines?
- **Lock-Free Data Structures**:  
  - Pros: Lower contention in theory.  
  - Cons: More complex to implement and debug. The performance gain for our expected workload (chunk‐mesh jobs that take milliseconds each) is minimal compared to the simplicity of a `std::queue` + `std::mutex`.
- **Containerized Job System (e.g., job graph, fiber pools)**:  
  - Overkill for Phase 1. We only need to queue coarse‐grained tasks (one chunk mesh per job).  
  - Java‐Minecraft itself uses a simple thread pool for chunk meshing. We mirror that model.
- **C++20 Coroutines**:  
  - Coroutines could make some asynchronous flows more ergonomic (e.g., “co_return mesh data”), but integrating them with OpenGL context and GPU upload is more complicated.  
  - A plain thread pool with `std::function<void()>` is adequate and works on all compilers supporting C++17.

## Consequences
- **Pros**:  
  - Clear, easy to reason about.  
  - Uses only standard C++17 features (`<thread>`, `<mutex>`, `<condition_variable>`, `<queue>`, and `<functional>`).  
  - Portable across Windows, macOS, Linux without additional dependencies.  
  - Matches Java‐Minecraft’s design principle (thread pool for chunk meshing).
- **Cons**:  
  - Under extremely high job‐submission rates (e.g., thousands of very short jobs per frame), mutex contention might become a bottleneck.  
  - We do not exploit lock‐free data structures or advanced scheduling (e.g., work stealing).  
  - Coroutines could reduce boilerplate for asynchronous logic but are not necessary for Phase 1.

## Review and Next Steps
- If profiling shows that the `std::mutex` + `std::queue` becomes a throughput limiter, consider switching to a lock‐free queue (e.g., moodycamel’s ConcurrentQueue) or adding a simple work‐stealing layer on top.  
- For now, leave the implementation as is. In Phase 2, we will enqueue chunk‐mesh tasks at ~10–20 per second (one per newly visible chunk), which is well within the capacity of a small thread pool.

---
