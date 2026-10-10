# Quad records: one 16-byte record per cube face (2026-10-08/09)

Status: IMPLEMENTED 2026-10-09 behind `OBEY_QUAD_RECORDS=0` (mesher switch),
measured on the M4 Air still frame — see "Measured results" at the end and
engineering-notes (GPU structural work, step 3). Planned 2026-10-08, after the sprite array shipped and the 16-byte
vertex was declined (every 16-byte packing of today's per-corner vertex loses
either light or geometry precision — see engineering-notes, GPU structural
work). This design loses neither: it stops sending per-corner data for the
geometry that has none.

## The idea

A terrain quad is drawn from four 20-byte `TerrainVertex` corners plus six
32-bit indices: 104 bytes. For a full cube face every corner is derivable from
the block, the side and the corner number, and the sprite array gives the uv
from the corner number alone (tile space, hardware repeat). So a cube face
becomes ONE record the vertex shader expands: the vertex index says which
record and which corner (`gl_VertexIndex >> 2`, `& 3` under a shared quad
index pattern), the record is fetched from the slab's buffer texture (vertex
pulling, the face-map view the fragment shader already uses), and the
position is `origin + block + sideOffset[corner]`.

Geometry that is not a cube face — stairs, slabs, fences, doors, fluid plates
with their nine heights, the two-sided plant quads, rotated and offset model
elements — keeps today's vertex path untouched. Two streams per section layer.

## The record (16 bytes, one RGBA32UI texel)

Light is exact. A full cube face's smooth light is MC's four-cell average
(`LightCoordsUtil.smoothBlend`) of levels that are multiples of 16, so every
corner value is a multiple of 4: six bits per channel hold it bit for bit.
("Any value" light only comes from the partial-face weighted blend, which
never applies to a full face — those faces are in the vertex stream anyway.)
Emissive faces are 240/240, also multiples of 4.

Unmerged cube face (`mode` 0):

| bits | field |
|---|---|
| 12 | block x y z inside the section (4 each) |
| 3 | side (the facing group's code — the draw list sorts by it today) |
| 10 | section slot (the slab's origin-table row, patched at upload as now) |
| 2 | mode |
| 16 | sprite id (array layer; overflow ids keep the sprite-table path) |
| 8 | four AO corner codes, 2 bits each (tile-corner order as the face map) |
| 24 | tint × face shade, rgb (or tint alone with the shade from the side; same bits) |
| 48 | four corner lights, block6 | sky6 each, quarter levels |
| 5 | spare |

Greedy rectangle (`mode` 1): block origin 12, side 3, slot 10, mode 2, width 4,
height 4 (1..16), face-map record index 32 (the per-block records stay exactly
as today: colour, AO, sprite, four corner lights per block), 61 spare. The
rectangle's corners are `origin + (w, h)` along the side's axes.

Modes 2 and 3 are reserved (a two-sided cube-aligned thin face, say).

So a cube face costs 16 bytes against 104, and a greedy rectangle 16 bytes plus
its records against 104 plus its records. The uv costs nothing: corner
(0/1, 0/1) × (w, h) in tile space, which is what the sprite-array path samples.

## Expected results

Measured inputs (tour at RD 32, 2026-10-08): the greedy census counts 4.7 M
cube-face candidates meshed over a run, 46 % of them multi-block rectangles,
54 % single faces; the still frame draws ~1 M vertices from up to 10 slabs; an
opaque slab is 512 k vertices (9.8 MB) + 1 M indices (3.9 MB). Cube faces are
the large majority of quads in natural terrain; the rest (plants, water,
stairs, fences) stays in the vertex stream.

- **Memory:** cube-face geometry −85 % (104 → 16 B a quad, no index buffer for
  the stream). Terrain slabs overall roughly −60..−70 % at RD 32 (the free
  stream and the face-map records remain). Device memory at RD 32 drops by
  tens of MB per occupied slab; the exact figure is the first thing the census
  step below prints.
- **Streaming:** mesher output and uploads shrink by the same factor —
  `UploadSection` / `FinalizeUpload` bytes, the saved-view load and the
  join hand-over get faster (measure with the chunk-streaming replay).
- **GPU, this Mac (M4):** no frame-rate change expected. The fragment stage is
  the critical path (every vertex-only saving so far read as null live: Tier 2's
  vertex trims, the half varyings). The vertex stage does a few more integer
  ops per vertex (decode + corner table) and fetches 16 bytes per quad instead
  of 80.
- **GPU, bandwidth-bound machines (Intel iGPU):** vertex fetch bandwidth
  −80 % for cube faces; that is where the half varyings' saving lives too.
- **CPU:** draw count up to +15 % where a section has both streams (two
  draws per run); `MergeRuns.Flush` is at the API floor per draw. Mesher time
  slightly down (fewer bytes written).
- **Risk to the look:** none by construction — positions on the block grid are
  exact (better than today's 1/2048 rounding), light bit-exact, uv exact.
  Parity: panoramas against the pre-change build, within the same-build control.

## What could cost frames (the negatives, honestly)

- **More draws.** A section with both streams needs two draws per run instead
  of one: up to +15 % draws where stairs, plants or water share a section with
  cube faces. Metal took +57 % sub-draws for a gain in the `OBEY_SPLIT_MIN`
  sweep, and `MergeRuns.Flush` costs ~0.12 µs a draw on the CPU, so this is
  small but not zero. Mitigation: draw all record runs of a pass, then all
  vertex runs, so the pipeline switches twice per pass, not per section.
- **Vertex pulling vs fixed-function fetch.** On Apple GPUs the vertex fetch is
  shader code either way (stage_in compiles to buffer loads), so a texel
  fetch per vertex is the same work; on some desktop GPUs pulling can be
  marginally slower than the dedicated fetch path. The decode is a handful of
  integer ops per vertex. Both sit in the vertex stage, which overlaps the
  fragment stage here, so the live effect on this Mac should be nil either way.
- **Translucent stays on vertices.** Translucent quads are sorted back to front
  per section in one list; glass (a cube face) and water (a free plate) in one
  section would otherwise interleave across two pipelines and lose their
  order. The translucent layer is ~5 % of the vertices, so it keeps today's
  format and the records cover opaque and cutout only.
- **Nothing on the fragment side changes:** same pixels, same shaders, same
  overdraw, same hidden-surface removal. The look is identical by
  construction.
- **Expected frame-rate band on this Mac: −1 % to +1 %**, i.e. the
  measurement's noise; the series is there to catch a surprise, not to find
  a win. The win is memory, streaming and weaker GPUs.

## Work plan

1. **Census** (half a day). Per section layer, count quads by kind: cube face
   unmerged, greedy rectangle, free (and why free: model shape, fluid, two-sided,
   random offset, rotation). Print bytes today vs projected. Go/no-go on the
   number; also tells whether `mode` 2/3 are worth defining.
2. **Record format and mesher.** `QuadRecord` next to `TerrainVertex`
   (`core/Vertex.hpp`); the mesher's cube-face path (`FlushGreedyQuads` and the
   unmerged full-face emit) writes records; everything else keeps emitting
   vertices. Section mesh output gains a record array per layer.
3. **Mega buffer.** The slab holds, per section layer: records, free vertices,
   face-map records; one buffer, 16-byte aligned runs, the face map becomes an
   RGBA32UI view (one texel = one quad record or one block record; the
   fragment shader's two 8-byte fetches become one 16-byte fetch and shifts).
   A shared static quad index buffer (pattern 0 1 2 2 3 0 + 4q, uint32, sized
   for a slab's maximum quads) replaces per-section indices for the record
   stream; `baseVertex` = 4 × first record. Draw lists and `MergeRuns` carry
   two streams per run; facing groups work unchanged (a record has a side).
4. **Shaders and backends.** A record vertex shader (`terrain_rec_vk.vert`,
   GL twin, f16 variant) with NO vertex input: it reads the record from the
   face-map texel buffer (bound to the vertex stage too — Vulkan's texel-buffer
   set layout gains `VERTEX_BIT`; Metal already binds both stages) and emits
   the same varyings as `terrain_vk.vert`, so the three fragment shaders are
   shared. Backends: a pipeline with zero vertex bindings (Vulkan: empty
   vertex-input state; Metal: no vertexDescriptor; GL: an attribute-less VAO).
   Generator: nothing new (set 4 binding 0 already in BINDINGS).
5. **Shader packs** (OpenGL). A pack's `gbuffers_terrain` reads classic
   attributes; rather than teaching the prologue to expand records, the mesher
   emits classic vertices while a pack is active (a pack load already remeshes
   everything), so packs see exactly today's data.
6. **Measure.** Memory (`Mtl/DeviceAllocMB`, the slab log line) and streaming
   (replay with chunk loading) are the headline; frame rate by the cross-build
   alternating plateau series (a mesh format cannot be toggled per phase) plus
   a same-pose Xcode capture for the vertex-stage bytes; parity panoramas.
   Kill switch: `OBEY_QUAD_RECORDS=0` keeps the mesher on vertices.

Order of magnitude: three to five working sessions. Steps 2–4 must land
together; 1 and 5 can be done first and last.

## Other "math tricks" considered for the bytes that remain

For the free vertex stream (the 20-byte corners that stay):

- **Face shade from the side**, not stored: saves nothing per vertex (the
  tint byte count is the same) but makes the record's tint the pure biome
  tint — kept as a note for the record format.
- **Tint as a palette index.** Most faces are untinted white; the rest are a
  handful of biome colours per section. An 8-bit index into a per-section
  palette would save 16 bits a vertex, but the palette is another per-section
  table to upload and bind (the origin table has no room) — not worth it while
  the free stream is the minority.
- **Light as an index** instead of two coordinates: no — the lightmap is a
  2D table of both, there is no smaller exact code for a 0..240 pair.
- **Position**: the section-relative 16-bit fixed point is already the exact
  form for grid geometry; the only way down is precision, declined.

Nothing there is worth doing before the record stream exists; after it, the
free stream is small enough that its 20 bytes stop mattering.

## Measured results (2026-10-09, M4 Air, still frame, RD 32, clear)

What shipped matches the plan: `Render::QuadRecord` (Vertex.hpp), records in the
slab's RGBA32UI face-map view, `terrain_rec*.vert` pulling vertices, the shared
quad index pattern, two streams per opaque/cutout layer, translucent on vertices.
Census: 1.65 M rectangles + 1.89 M single faces as records, 0 single faces kept as
vertices for light (every cube-face light is on quarter levels in practice).

| | records off | records on |
|---|---|---|
| fps (clean pairs) | — | −1.0 / −1.4 % (one −8.7 % first-run outlier) |
| `Gpu/EncFrameUs` | — | +1.4–2.1 % |
| `Mtl/Draws` | 5861 | 6827 (+16 %, the second stream) |
| `Mtl/DeviceAllocMB` | 764 | 657–684 (−80..−107 MB) |
| opaque pool | 10 slabs | 9 slabs, 6.9 MB used |

Findings: the one-texel face-map change alone is ≈ −1.5 % fps; smaller slabs
(128 k opaque) raise sub-draws +9 % with no memory gain, because the opaque pool
is slot-bound (1024 sections a slab) once the cube faces leave the vertex stream
— reverted to 256 k. Look parity against the same build with records off:
structurally identical, ≈ 2× the control's noise (mean ≤ 0.7/255), streaks on
distant leaves from single faces now sampling the sprite array via the record
path. The expected "fps nil here" held; the expected memory saving arrived at
device level (−10..14 %), smaller than the per-slab estimate because slots, not
bytes, bound the opaque pool and the free stream keeps its slabs.

Tried on top (2026-10-09): a rectangle's one sprite carried in the record so
the fragment shader could sample it without waiting for the block record
(the greedy scan merging only equal sprites). Null on frames (−0.5 % ± 2.6 %),
+9 % rectangles, +28 MB — reverted; see engineering-notes, "Fragment stage,
round two".
