# Shader packs on Vulkan and Metal (started 2026-10-09)

Status: METAL DONE 2026-10-09 (Sildur's Vibrant renders right); VULKAN IN
PROGRESS. The OpenGL pipeline (`Render::ShaderPipeline`,
`docs/engineering-notes.md` "Shader packs") is the reference; this is the
plan for running the same packs on the Vulkan and the native Metal backends,
written before the work so each phase has a gate.

## Why it is a port, not a switch

A shader pack is compatibility-profile GLSL (`#version 120`, `gl_Vertex`,
`gl_FragData[n]`, loose uniforms set by name, texture units by number). The
OpenGL pipeline rewrites it to core GLSL and hands it to the driver. Vulkan
and Metal have no GLSL compiler, no loose uniforms, no `glUniform` by name,
no numbered texture units, and this engine's two backends bind a FIXED
layout (the Common block, six texture slots, push constants) baked into
every shipped `.spv`/`.metal` at build time. So:

1. a GLSL compiler at run time — **glslang** (Vulkan GLSL → SPIR-V);
2. **SPIRV-Cross** at run time for Metal (SPIR-V → MSL) and for reflection
   (the uniform block's member offsets, the sampler bindings) on both;
3. a second translation target in `ShaderPackGlsl` — Vulkan GLSL: every
   non-opaque uniform gathered into one `std140` block, every sampler at
   an explicit `(set, binding)`, every varying at an explicit location;
4. a backend contract for "pack programs": `CreatePackShader` (SPIR-V in,
   its own descriptor/argument layout), uniforms written by name into the
   program's block, sixteen texture slots, render targets of up to eight
   colour attachments in the pack's formats, depth textures sampled, depth
   copies between targets, the shader-override mode the GL backend has;
5. the pipeline itself freed of its OpenGL gate.

Coordinate conventions: both backends draw with the y-flipped, 0..1-depth
projection (`kZeroToOneDepthCorrect`), so every render target is upside
down relative to GL — consistently. The packs get the SAME flipped matrices
as `gbufferProjection`/`gbufferModelView`, so their position reconstruction,
sun-to-screen projection and TAA reprojection stay self-consistent; the
depth values themselves equal GL's (`(z_ndc + 1) / 2`), so `depthtex`
arithmetic needs no change. The one place "up" is assumed on screen is the
final blit to the window, which is flipped too.

## Phases and gates

| Phase | What | Gate |
|---|---|---|
| 1 | glslang + SPIRV-Cross via FetchContent (`SHADER_PACK_SPIRV`, static, tests/tools off); `Shaders::PackCompiler`: Vulkan GLSL → SPIR-V, reflection, MSL. `ShaderPackGlsl` Vulkan target. | Sildur's 12 programs compile to SPIR-V at load on Metal, log says so; reflection lists every uniform the GL path sets. |
| 2 | Backend contract: `CreatePackShader`, `SetPackUniform*` by name (a per-program CPU block + ring upload), 16 texture slots for pack programs, pipelines keyed by the bound target's attachment formats. Metal first (the user's renderer), Vulkan second with the same SPIR-V. | A composite-style full-screen pass draws through a pack program into the window. |
| 3 | Render targets: `CreateRenderTargetFromTextures(colors[≤8], depth)` in the pack's formats (RGBA8/16F/32F, R11G11B10…), sampleable depth textures, `BlitRenderTargetDepth`, `SetShaderOverrideMode` (engine draws take the pack's program for their family), `ReadDepthPixel` for the probes. | The gbuffers pass writes colortex0/1/2 and the deferred pass reads them. |
| 4 | `ShaderPipeline` on the new contract: load path, uniforms, inputs, the terrain/entity families through the chunk renderer's pass overrides, the shadow pass, the final pass to the window. | Sildur's renders on Metal; screenshot parity with the GL pipeline at the still pose. |
| 5 | Vulkan: the same programs as SPIR-V modules; MoltenVK quirks. Then the known gaps shared with GL (hand through the pack, entities in the shadow map, weather). | Sildur's renders on Vulkan. |

## Decisions

- One code path for both backends above the RenderBackend line: the
  pipeline sees "pack programs" and slots, never SPIR-V or MSL.
- Uniforms by name stay the pipeline's API (`SetUniform*(shader, "name")`):
  for a pack program the backend writes the program's block at the
  reflected offset instead of routing into the Common block.
- glslang and SPIRV-Cross are fetched at the pinned tags and built static;
  the launcher and the OpenGL-only configurations do not pay for them
  (`SHADER_PACK_SPIRV=OFF` leaves the Vulkan/Metal pack path out, with the
  status line saying so).

## What the port taught (2026-10-09)

Status: Sildur's Vibrant renders the same on OpenGL, Metal and Vulkan
(window captures compared side by side; water with reflections was the last
difference). Each item below cost a round of debugging; the probes that
found them are in `tools/shader_pack_probe.sh`.

- **Clip space stays GL-style in the pack's matrices.** The engine hands
  GL-style matrices around and the Vulkan/Metal backends premultiply
  `kZeroToOneDepthCorrect` into every `uMVP` they are given. A pack program
  cannot take that: its own depth arithmetic (`gbufferProjectionInverse`
  on `depth * 2 - 1`, the water's ray march) assumes GL matrices. So every
  matrix a pack sees is GL-style, and the translated vertex stage remaps
  clip z itself after the pack's main (`gl_Position.z = (z + w) / 2`,
  `ShaderPackGlsl::Vulkanize` wraps the core translation's own main). The
  stored depth then equals OpenGL's `(z_ndc + 1) / 2`. The first attempt
  (remapped matrices) clipped the near half of the world and broke every
  reconstruction.
- **Pack texture targets are stored the OpenGL way** (row 0 = scene
  bottom): both backends flip the viewport (and the front-face winding) for
  every draw into a texture target, and the final pass to the window draws
  upright. A pack samples its targets with GL's `v` and reads
  `gl_FragCoord` bottom-up; with only the final blit flipped the picture
  was upside down.
- **Sampler units are baked at translation**, so everything that decides a
  unit must be known before a program compiles: the colour buffers the
  gbuffers programs sample (`ScanGbufferColorReads`, now before the compile
  loop). Compiled early, `colortex4` took a free unit (15) instead of its
  bound one (13) — black water on Metal, red on Vulkan, and OpenGL never
  noticed because it assigns units by name afterwards.
- **A program may read a buffer it draws into** (Sildur's water reads
  `colortex4` for reflections while writing it). OpenGL tolerates the
  feedback; Vulkan and Metal do not (Vulkan validation, Apple tile memory).
  The pipeline snapshots such buffers before the translucent pass
  (`ColorBuffer::feedback`, `RenderBackend::CopyTexture`) and binds the
  copies; the gbuffers stage's `depthtex0` is the depth snapshot for the
  same reason.
- **Fast math, and only one NaN.** Both backends compile with fast math;
  so, in effect, does the OpenGL driver. The one divergence found was
  `pow(0.0, 2.2)` (exp2(y·log2(0)) → NaN) in a water fog term — black water
  on Metal, red on Vulkan. The translation defines `pow` with a base
  clamped to 1e-7. Precise math is NOT the answer: SPIRV-Cross turns the
  float-controls execution mode (and Metal's precise function mode) into
  `precise::` transcendental calls, which made the pack's deferred passes
  5× slower on both backends (an afternoon's regression, found through
  the dumped MSL).
- **Translation corner cases** from one pack: `varying`/`attribute`
  spellings, several declarations on one line and code after them, a
  sampler with no free unit (the composite layout uses all 16 — Distant
  Horizons' `dhDepthTex0` is #define-aliased to a same-type sampler of the
  same stage), `layout(std140) uniform SectionOrigins` → set 3 binding 0,
  fragment inputs the vertex stage declares only under another `#if`
  branch (reflected after the compile, vertex outputs appended, the stage
  recompiled), pack attributes the engine never feeds (input locations from
  8).
- **Pipelines per vertex layout.** One pack program stands in for several
  engine shaders with different layouts: both backends key the pipeline by
  (program, overridden engine shader), describe every attribute the program
  reads (a missing one as four bytes at offset 0, OpenGL's default) and
  drop the ones it does not.
- **Slot collisions on Metal and Vulkan.** The chunk renderer binds 2D
  arrays on slots 4/5, where the gbuffers layout keeps `normals`/`specular`;
  OpenGL keeps a unit's 2D and array bindings apart, these backends have one
  index per slot, so each remembers the last plain 2D texture bound per
  slot and binds that for a pack sampler2D (`PackShaderDesc::sampler2DSlots`).
- **The deferred passes ride the translucent pass's hook** (the chunk
  renderer's before-translucent hook: depth snapshots, `RunDeferred`, the
  feedback copies). The pass skipped itself — hook included — whenever no
  visible section had translucent geometry, so a view of sky alone, or the
  sun over dry land, never got the pack's sky: colortex4 stayed black and
  composite1's godrays drew their orange halo over black across the whole
  screen (every backend; "Vulkan looks right" was a view with water in it).
  The hook now runs before that early-out.
- **Weather (2026-10-09 evening).** `rainStrength`, `thunderStrength` and
  `wetness` were hard-coded 0 — rain looked like a clear day. They now come
  from the client's rain/thunder levels at the weather's partial tick
  (`ShaderFrameInput`), `wetness` eased with the pack's `wetnessHalflife` /
  `drynessHalflife` (ticks, Iris's SmoothedFloat). Rain and snow go through
  the pack's `gbuffers_weather` family (render stage 21): a pack's vertex
  stage only transforms MC's quads, so while a pack is active the weather
  renderer draws MC's quads at full resolution (no streak instancing, no
  Rain Resolution: Half — `SetShaderPackActive`) and names its particle
  program's handle (`PackShaders`) for the family, since its sources are
  the particle system's and a source match could not separate them.
- **Metal 4** has no hazard tracking: a render encoder waited for earlier
  fragment work only when it loaded an attachment, so a pack pass that
  clears its target and samples the previous pass (and the final pass into
  the window) read the composites before they were drawn — a black frame.
  A frame that binds a pack program (`m_packFrame`) makes every later
  render encoder wait for prior fragment work.
- **Metal:** depth textures created with data are cleared to that depth by a
  render pass (no buffer→depth copy); a texture target cleared and dropped
  in one frame must keep its pass objects until the frame retires (Vulkan's
  deletion queue; a `vkDeviceWaitIdle` does not cover an unsubmitted
  command buffer — MoltenVK crashed at submit).
- **Dev switches** (any backend): `OBEY_PACK_SHOW=<buffer>[@gbuffers|@deferred]`
  shows a pack buffer in place of the final image; `OBEY_PACK_PROBE=<program>:<glsl>`
  makes a program's first output `vec4(<glsl>)` — any global of its fragment
  stage; `OBEY_PACK_DUMP=1` writes the translated GLSL (and `=msl` the Metal
  source) to `shaderpacks/.translated/`, `=block[:<program>]` logs the uniform
  writes (Metal); `OBEY_SHADER_PACK=<id>|none` picks the pack for one run;
  `OBEY_VK_VALIDATION=1` (with the SDK's `VK_LAYER_PATH`) turns the Vulkan
  validation layer on in any build. Window captures by id, not focus:
  `tools/shader_pack_probe.sh`.
- **Where a pack frame goes on Metal** (2026-10-09 evening, Xcode capture
  of a Sildur's frame at the horizon pose, RD 12, 3420×2146, Medium state
  19.6 ms serial): the pack's own full-screen passes are ~85 % of the GPU
  — deferred.fsh 27 % (168 registers), composite.fsh 17 % (bloom /
  godrays / VL), composite1.fsh 9 %, composite2.fsh 9 % (TAA), the water
  6 %, the shadow pass 5 %; the engine's own share is ~5 % (texture flush
  1.1 %, the colortex4 feedback copy 1.1 %, the depth copies 0.8 %, the
  Frame encoder 4.8 % for the hand and GUI). Live (Apple's HUD): 27 ms GPU
  per frame, 50 fps. Three engine-side cuts were made and measured null
  live (fewer blit encoders, GPU frame unchanged — the HUD's "blit 7.4 ms"
  was overlapped waiting, not work): depthtex2 aliased to depthtex0 (our
  hand is never in the pipeline's depth; `OBEY_PACK_DEPTH2=1` restores the
  two copies), feedback snapshots only for buffers a gbuffers program both
  samples and writes (`OBEY_PACK_FEEDBACK=all`), and composite targets
  bound without loading their colour (`RenderBackend::BindRenderTargetOverwriting`,
  Metal DontCare; `OBEY_PACK_LOAD=1` keeps the loads). Kept as correct
  hygiene (~90 MB/frame less traffic), not as a speed-up. The texture
  flush is ~250 small animated-sprite copies a frame, the same on the
  plain path. The real levers are the pack's options (TAA, bloom/godrays,
  shadow samples) and Render Resolution.
- **Compile side, measured null** (same evening). deferred.fsh runs at 168
  registers / 27 % occupancy, F32 limiter 95 %: ALU-bound by its own math.
  The translated MSL is plain SPIRV-Cross output (every function
  always_inline, one constant-address offset table, no precise, no
  conversions worth the name). `OBEY_PACK_MSL=noinline|size|mathapi`
  (strip the forced inlining; the size optimisation level; the macOS 15
  math-mode API) against the default, still frame, Tracy per-encoder
  zones: noinline equal (the Metal compiler inlines anyway), size −9 %
  fps, mathapi equal (fastMathEnabled already is fast math + fast
  functions). glslang runs without SPIRV-Tools (no optimizer) and the Metal
  compiler's own LLVM makes that moot. The offline native compiler gives
  no static register count without an undocumented translator script
  (engineering notes, "Registers to 32"). Registers move only with a
  structural cut to the pack's shader — not the engine's call.
- **Cost** (M4 Air, RD 12, 3420×2146, vsync): Sildur's on Metal runs at
  36–38 fps cooled, the same as before the port's later changes; Vulkan
  within a frame or two of it; OpenGL 17–19. The pack's deferred and
  composite passes dominate on every backend; Render Resolution remains the
  lever. The Air's GPU runs ~2× slower after hours of back-to-back runs —
  the 8–19 fps readings of the afternoon were precise math plus heat.
