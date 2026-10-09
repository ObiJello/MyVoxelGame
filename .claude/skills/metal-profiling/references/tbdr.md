# TBDR rules applied to ObeyCraft's Metal pass graph

Apple's reference: the installed `game-porting-skills` plugin, `skills/translating-to-metal4-api/references/tbdr-architecture.md` (bandwidth cost model, patterns) — read it first. This file maps it onto this engine.

## The pass graph at the user's settings (OIT off, half-res rain on, render scale 100 %)
1. Optional blit encoder: queued texture updates (animated atlas, lightmap, maps) — on the "Uploads" or "Frame" command buffer.
2. "Target N (1710x1073)": the half-res rain pass, drawn *ahead of the frame* from the previous frame's depth (Load/Store colour; Load/Store depth).
3. "Frame": sky → opaque terrain → cutout terrain → entities → translucent terrain → particles/clouds → weather composite (full-screen blend of the rain target) → held item → HUD. Colour Clear/Store; depth+stencil Clear, Store only when preserved for the next frame's rain.
Attachments: colour BGRA8 3420×2146 (28 MB), depth D32S8 (37 MB per slot); mailbox stand-in (28 MB) for unpresented frames.

## The five rules and where this engine stands

| Rule | Status | Lever |
|---|---|---|
| Fewer, larger passes; never split a pass that shares attachments | 2 encoders/frame in normal play; with OIT on, an *empty* frame pass is opened and closed between every OIT pass (Load+Store of colour + D32S8, up to 6×) — `MetalPipelines.mm OitBeginPass/OitEndPass`; the same pattern in `SuspendFrameForCopy`, `ResolveScaledScene`, `EndFrame` | Resume the frame pass without `EnsureEncoder` when nothing was drawn; plot encoders/frame |
| `DontCare` stores / memoryless for transients | Frame depth: Store when preserved else DontCare, but the texture is private, not memoryless; render-target depth: always Load/Store and never sampled; stand-in colour: always Store | Memoryless depth when not preserved (Apple GPUs only); DontCare for the stand-in on unpresented frames; RT depth DontCare/memoryless |
| `Clear`/`DontCare` loads unless prior contents are needed | Render targets always Load (rain target, outline, post chain) | Clear where the owner clears anyway |
| Opaque → feedback (cutout/discard, depth-writing with alpha) → translucent | Order is right (opaque, cutout, translucent); cutout at 25 % serial cost disables hidden-surface removal for its pixels | Keep cutout minimal (leaf/foliage coverage), `[[early_fragment_tests]]` where the shader never writes depth and has no discard |
| Write all attachments, no partial masks | Colour-write-disabled draws exist (portal passes) | Confirm they are rare in the main pass |

## Bandwidth arithmetic for this frame size
One full-screen attachment at 3420×2146: 7.34 M px → BGRA8 29 MB, D32S8 37 MB. A store+load round trip of colour+depth = ~130 MB; at 200 fps that is 26 GB/s of the M4's ~120 GB/s. Each avoidable encoder split is worth measuring with the HUD encoder timing before and after.

## Hidden-surface removal and launch limiters
On Apple GPUs opaque fragments are shaded once per pixel regardless of draw order ("PreZ Test Fails" counts the rejects). Passes that are *launch-limited* (Fragment Shader Launch Limiter ≫ ALU) are bound by how many fragments start, so the remedies are fewer covered pixels (resolution, scissor, column culling) and fewer, larger primitives — not cheaper shader code. The half-res rain target is this engine's clearest case (93.7 % launch limiter, 12× overdraw, ~23 k px per primitive).
