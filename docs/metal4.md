# Metal 4 runtime path

Metal 4 (macOS 26, Apple silicon; `MTLGPUFamilyMetal4`) is a redesign of the
command side of the API: reusable command buffers filled from per-frame
allocators, argument tables instead of per-encoder `set*Buffer/Texture`
calls, explicit residency sets, an explicit compiler, commit feedback in place
of completion handlers, one compute encoder for blits and dispatches, and no
automatic hazard tracking between passes. It is a separate backend path, not
a flag on the Metal 3 one, and it is a **runtime** choice: the shipped build
keeps the 12.7.6 deployment target, Metal 4 symbols are reached only inside
`@available(macOS 26.0, *)`, and a Mac without it (Intel, or any pre-26
macOS) runs the Metal 3 backend exactly as today (user requirement,
2026-10-07). Apple's reference for every translation decision is the
`translating-to-metal4-api` skill (`game-porting-skills` plugin) and the
`MTL4*.h` SDK headers.

## Selection

The default since 2026-10-09: `--metal` (the launcher's "Metal") takes the
Metal 4 path wherever the device reports the family under macOS 26, the
Metal 3 backend elsewhere. `Render::CreateRenderBackend(Metal)` builds
`Metal4Backend`, whose `Initialize` falls back to the Metal 3 path when the
Metal 4 objects cannot be made, and logs which runs; `--metal3` or
`OBEY_METAL4=0` keep Metal 3 (the A/B, a driver problem), `--metal4` is the
old opt-in and now a no-op. `Metal4Backend` derives from `MetalBackend`:
resources (buffers, textures, samplers, the metallib, pipelines' MSL) stay
the base's, the command path is overridden milestone by milestone, so the
game runs on it at every step.

## Milestones

0. **Scaffold** (2026-10-08): flag, detection, fallback, the Metal 4 objects
   created at `Initialize` — `MTL4CommandQueue`, one `MTL4CommandAllocator`
   per frame slot, `MTL4Compiler`, a `MTLResidencySet` attached to the queue —
   and every frame still recorded through the inherited Metal 3 path.
1. **Command recording**: one reusable `MTL4CommandBuffer` per frame slot,
   `beginCommandBufferWithAllocator` after the allocator's `reset`, the frame
   pass from a `MTL4RenderPassDescriptor` (explicit size), `endCommandBuffer`,
   `commit:count:options:` with `MTL4CommitFeedback` for the GPU start/end
   times the frame pacing and the `Mtl/*` plots read today from the handlers;
   presentation with `waitForDrawable` / `signalDrawable` on the queue and the
   mailbox decision unchanged. Memoryless depth, the stand-in, the late depth
   bands and the three-slot pacing carry over as they are.
2. **Binding**: `MTL4ArgumentTable`s (vertex, fragment) sized for the
   `MetalBindings` indices, filled with buffer GPU addresses and texture /
   sampler resource IDs — the uniform ring, section origins, push-constant
   block (a ring window: there is no `setBytes`), textures per slot. Every
   buffer and texture joins the residency set on creation and leaves on
   destruction (`addAllocation` / `removeAllocation`, `commit` once per batch);
   the uniform ring and staging buffers are long-lived members.
3. **Pipelines**: the same metallib functions through
   `MTL4LibraryFunctionDescriptor` + `MTL4RenderPipelineDescriptor` on the
   compiler (async build allowed), the manifest warm-up as today, a
   `MTL4PipelineDataSetSerializer` on disk later if first-launch builds ever
   show up in `Mtl/PipelinesBuilt`.
4. **Uploads and copies**: the Uploads/Readback/Texture-flush blits become
   `MTL4ComputeCommandEncoder` copy commands with explicit barriers
   (`barrierAfterStages:beforeStages:` — no automatic hazard tracking): the
   staging ring, per-frame texture copies and the frame-depth hand-off each
   get their barrier.
5. **Instrumentation**: per-encoder times from `MTL4CounterHeap` timestamps
   (`writeTimestampIntoHeap`), commit feedback for the frame span, the same
   Tracy plots and GPU zones, capture scope unchanged.
6. **Validation and A/B**: `MTL_DEBUG_LAYER=1` run, panorama parity against
   the Metal 3 path, the still-frame paired test (`STILL=1`, 2 s periods) and
   the tour; default on supported Macs only if it wins; `--metal3` (or the
   flag's absence) keeps the old path selectable for players.

## Status (2026-10-08)

Every milestone is built and runs (`MetalFour.mm`, the `m_metal4` branches
in `MetalBackend.mm` / `MetalPipelines.mm` / `MetalResources.mm` /
`MetalInstrumentation.mm`):

- Command recording: reusable Metal 4 command buffers from per-slot
  allocators (frames and uploads, the uploads round-robin behind
  `uploadsFree`), render encoders from converted descriptors with a
  consumer barrier (prior copies; prior fragment work only for a pass that
  loads an attachment), compute-encoder copies with scoped barriers, commit
  feedback for completion, GPU span and errors, presentation through the
  queue's `waitForDrawable` / `signalDrawable`.
- Binding: two argument tables (vertex, fragment) with GPU addresses and
  resource IDs; push constants, the OIT parameters and the clear quad's
  parameters as uniform-ring windows; indexed draws by address.
- Pipelines: `MTL4Compiler` from `MTL4LibraryFunctionDescriptor`s (the
  shader's library and function name), colour attachments with
  `MTL4BlendState`, no depth/stencil formats (the pass's).
- Residency: every buffer and texture joins the residency set at creation
  (memoryless images excepted) and leaves through a frame-tagged garbage
  list once the frame that could read it completed; OIT images included.
- Frame pacing: the queue signals a shared event with the frame number
  just before the frame commits — once everything before it completed —
  and a listener opens the gate for the frame after it (at most one frame
  queued behind the executing one). Metal 4 has no scheduled handler, so
  this gate is "previous frame completed", stricter than Metal 3's
  "previous frame scheduled"; `OBEY_MTL_PACING=0` and the `pacing` phase
  token measure it against the unpaced three-deep queue.
- Timers: a `MTL4CounterHeap` of timestamps per slot, the same sample
  layout as the Metal 3 sample buffers, every stamp written *inside* the
  encoder: Precise `afterStage:` timestamps before the first draw mark the
  vertex and fragment stage starts ("written before the stage begins",
  the SDK header), the same before the encoder ends mark their ends; a
  copy encoder gets a Precise timestamp after its barrier and one before
  it ends (`EndBlitEncoder`). Command-buffer timestamps mark the command
  processor — up to frames ahead of the execution — and mixing them with
  stage stamps gave spans of seconds (2026-10-08). The heap counts
  `mach_absolute_time` ticks (41.667 ns), not the sample buffers'
  nanosecond: the tick is the slope of the feedback's GPU start over the
  frame's first stamp from the first frame on (a long baseline, so the
  clocks' offset jitter averages out), snapped to the mach timebase when
  they agree within 2 %; a once-a-second `sampleTimestamps` anchor maps
  stamps onto the timebase Tracy's GPU context was created in.
- ImGui overlay: a Metal 3 command buffer after the frame, waiting on an
  `MTLEvent` the Metal 4 queue signals, draws the overlay over the
  drawable and presents it (`OBEY_DEBUG_UI=1` opens the overlay at start
  for a scripted run).
- F3 shows "Metal 4" or "Metal 3" in the system line.

Validation layer clean on the tour; still panoramas within the Metal 3
path's run-to-run noise. Measurements: engineering notes, "Metal 4 path".

## Known obstacles

- **ImGui** (`imgui_impl_metal`) records into a Metal 3 render encoder. In a
  Metal 4 frame it needs its own Metal 3 command buffer after the frame's,
  ordered with an `MTLEvent` (the queue's `signalEvent` / `waitForEvent`), or
  the dev overlay is drawn through the engine's own GUI renderer.
- **Vulkan** (MoltenVK) is untouched; the `_vk` shader source and the
  generator stay the single source of truth.
- Apple's guidance on what Metal 4 buys on a TBDR GPU: lower CPU cost per
  draw and bind, explicit control of residency and hazards. The engine is
  GPU-bound at render distance 32 on the M4 Air, so the honest expectation
  is better frame-time consistency and CPU headroom, not a large fps gain —
  the still-frame test decides.
