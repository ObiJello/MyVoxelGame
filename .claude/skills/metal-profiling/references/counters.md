# Apple GPU counters: what each limiter means and what to do

Definitions are Apple's own, from Instruments' `GPUPlugin.xrplugin/Contents/Resources/GPUCounterGraph.plist` (Xcode 26.2). "Limiter" = time the unit was *attempting* work (busy or stalled) as % of its peak; "Utilization" = time it *executed* work. A unit with limiter ≫ utilization is stalled on something else. Remedies are from WWDC20 10603 "Optimize Metal performance for Apple silicon" and the 2023 tech talk on M3/A17 counters.

**Top Performance Limiter** = max of ALU, Texture Sample, Texture Write, Buffer Read, Buffer Write, Threadgroup/Imageblock Read and Write. Look at the top one first; a frame with no limiter above ~40 % is not bound on one unit (then look at launch, occupancy and geometry).

## Shader execution

| Counter | Apple's definition | Act on it |
|---|---|---|
| ALU Limiter / Utilization (also VS/FS ALU) | Time ALU work is attempted / executed, % of peak ALU | Prefer half (F16), fast-math, approximations, remove implicit conversions, hoist per-vertex math out of the fragment stage |
| F32 / F16 Limiter | Same per sub-pipeline; 100 % = that pipeline cannot take more instructions in parallel | Move work between F32 and F16 so both run in parallel |
| Integer and Complex / Integer and Conditional Limiter | Integer, transcendental, conditional pipelines | Fewer integer decodes per fragment (pack in the vertex stage), fewer branches |
| Control Flow Limiter | Branch/loop work | Flatten small branches; uniform branches are cheap, divergent ones are not |
| Instruction Throughput Limiter | Shader execution pipeline overall. "If low, you can push more ALU and non-ALU instructions from the sub-blocks that are not fully limited" | If one sub-block is at 100 % and this is lower, rebalance instruction mix |
| Vertex / Fragment / Compute Shader Launch Limiter | "The rate at which [stage] shader threads are launched into the shader cores or stalled launching by the shader core, normalized to the peak launch rate" | **Launch-rate bound**: too many tiny invocations, not expensive ones. Fragment: fewer covered pixels (overdraw, half-res, scissor), bigger primitives; Vertex: fewer vertices (index reuse, fewer sub-draws) |
| FS Occupancy / VS Occupancy / Total Occupancy | % of shader-core resources occupied by the stage | Low occupancy with a high launch limiter → register pressure (see allocated registers in the Shaders tab) or L1 eviction; M3+/M4: check Occupancy Manager Target, L1 Eviction, LLC, MMU in that order |
| FS Interpolation Limiter | Fragment input interpolation | Remove unused varyings, use `flat`, pack varyings |

## Texture

| Counter | Definition | Act on it |
|---|---|---|
| Texture Read Limiter / VS / FS | Sample/read attempts vs peak | Fewer samples per fragment, mipmaps present, lower anisotropy where it buys nothing |
| Texture Filtering Limiter | Filtering unit | Anisotropy level (avg aniso is reported), trilinear vs bilinear, 128-bit formats run at quarter rate |
| Texture Read Cache Limiter / Miss Limiter | Texture L1 cache traffic / misses | Smaller formats, better locality (mip selection — `textureGrad` with wrong gradients drops to small mips and thrashes), compression (lossless is on for private textures) |
| Texture Write Limiter | Writes incl. attachment stores | Smaller pixel formats, `DontCare` stores, memoryless transients |

## Memory

| Counter | Definition | Act on it |
|---|---|---|
| Buffer Read / Write Limiter (VS/FS) | Buffer traffic vs peak | Pack vertex data, smaller types, vectorize loads, avoid device atomics and register spills |
| ThreadGroup/ImageBlock Read / Write, ThreadGroup Atomic | Tile memory traffic | Fewer threadgroup atomics, SIMD reductions, 16-byte alignment |
| L1 Cache Limiter | Shader-core L1 | Smaller working set, locality |
| Last Level Cache Limiter / Utilization / Miss Rate | GPU LLC | Shrink working set, locality; miss rate drives MMU and device-memory traffic |
| MMU Limiter / TLB Miss Rate | Address translation after LLC misses | Fewer distinct pages touched per tile; bigger allocations in heaps |
| GPU Device Memory Read / Write Limiter | External memory at the current performance state | The bandwidth bill: attachment loads/stores, big vertex buffers; "Bytes Read/Written To Device Memory" gives the absolute number |

## Geometry and raster

| Counter | Definition | Act on it |
|---|---|---|
| Primitive Block Output Limiter, Primitive Blocks Memory Write Limiter | Tiling block busy binning / writing primitive blocks (vertex+index data) to memory | Fewer primitives reaching the tiler (CPU culling, bigger merged quads, index reuse — "Vertices Reused" %) |
| Shaded Vertex Read Limiter | Primitive processing reading shaded vertices | Smaller post-VS vertex (fewer/packed varyings) |
| Cull Unit / Clip Unit Limiter | Culling / clipping | "Primitives Culled (Back-Face / Zero-Area / Off-Screen / Guard-Band)" say why primitives die; back-face-culled % is work the CPU could have skipped |
| Tiling Block Utilization, Primitives Per Tile | Tiler | — |
| Depth Test / Load / Store Utilization | Depth units | "PreZ Test Fails" % = hidden-surface removal doing its job; depth store traffic → `DontCare`/memoryless |
| Fragments Rasterized per Primitive, Pixels per Vertex, Average Pixel Overdraw, FS Invocations per Primitive | Raster geometry | Overdraw = FS invocations ÷ pixels stored; > 2–3 in a pass worth a look; launch-bound passes with huge px/prim want fewer pixels, not cheaper shaders |
| FS Helper Invocations Inefficiency | Helper lanes for derivatives | Tiny triangles waste lanes; avoid `discard` before derivatives |

## Reading a capture in practice

1. Encoder counters (CSV): find the top limiter per encoder; note launch limiters and overdraw separately — they are the TBDR-specific tells.
2. Shaders tab: cost %, SIMD groups, allocated/high registers, spills per pipeline — a 64-register fragment shader halves occupancy versus a 32-register one on the same core.
3. Per-draw counters (slow): which draws inside an encoder own the limiter.
4. Per-line costs: only with a host-OS-targeted metallib (`-frecord-sources`).
5. Cross-check with the live HUD/encoder timers: a counter that looks bad in a serial replay may overlap away in a real frame.
