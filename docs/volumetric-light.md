# Volumetric light beams

`Render::VolumetricBeam` (`src/client/renderer/effects/VolumetricBeam.{hpp,cpp}`) draws cones
of light that you see *in the air*: light scattered by haze along each view ray, a glare
sprite at the source, and a pool of light where the beam lands on terrain. The Hush
lighthouse (`HushLighthouseRenderer`) is the reference user. The header has the maths.

## Using it

```cpp
#include "client/renderer/effects/VolumetricBeam.hpp"

bool MyRenderer::Initialize() { return VolumetricBeam::Acquire(); }   // shared, ref-counted
void MyRenderer::Shutdown()   { if (m_init) VolumetricBeam::Release(); }

void MyRenderer::Render(..., const glm::mat4& proj, const glm::mat4& view, ...) {
    BeamCone c;
    c.apex        = glm::dvec3(pos) + 0.5;        // WORLD position (double)
    c.direction   = {0, 1, 0};
    c.length      = 160.0f;
    c.startRadius = 0.4f;  c.endRadius = 0.6f;    // equal radii = a column
    c.coreColor   = {0.9f, 1.0f, 1.0f};  c.edgeColor = {0.3f, 0.9f, 1.0f};
    c.intensity   = 1.0f;  c.haze = 0.3f;
    c.cacheKey    = MyKey(pos, 0);                 // stable per beam → cached terrain raycast
    BeamGlare g;  g.position = c.apex;  g.color = c.coreColor;
    g.intensity = 0.4f + VolumetricBeam::Flare(c, VolumetricBeam::EyeFromView(view));
    VolumetricBeam::Get().Draw(&c, 1, &g, 1, proj, view);
}
```

- Call `Draw` once per source with all its cones and glares. It draws pools, then cones,
  then glares; each is additive (pools multiply the terrain), depth-write off.
- Block-entity renderers need nothing else. The dispatcher calls `BeginView()` at the top of
  every `RenderAll`. Anything drawing beams from another pass calls `BeginView()` once before
  its first `Draw` in that view.
- Positions are world space (`dvec3`). The module converts to render space
  (`RenderOrigin.hpp`) itself.
- Fog, Blindness and Darkness are handled for you. The frame's fog attenuates the beam like
  terrain, and denser atmospheric fog makes the haze thicker. Darkness dims it with the
  lightmap pulse. `fogFactor < 1` keeps a landmark beam readable beyond the fog.

## Parameters worth knowing

| Field | Effect |
|---|---|
| `haze` | Scattering density per block. 0.2 is a faint shaft; 0.42 is the Hush lighthouse. |
| `anisotropy` | HG `g`. Higher values make looking back down the beam at the lamp brighter. 0.6–0.7 reads well. |
| `mist` | Drifting noise amount. Keep it at 0.3 or above so the beam doesn't look like a solid cone. |
| `halfIntensityDistance` | Inverse-square falloff: the light is half as bright at this distance. |
| `fadeIn`, `fadeOutStart` | Grow out of the lens over `fadeIn` blocks, and fade over the last `(1 - fadeOutStart)` of the length. |
| `terrainClip`, `lightPool`, `terrainStart` | A CPU raycast along the axis ends the cone at terrain and places the light pool. `terrainStart` skips the lamp's own housing (the lighthouse uses 3.5 for the glass lantern room). |
| `cacheKey` | Non-zero: the raycast reruns only when the beam moves, and otherwise every 8 views. |

## Backends

| | OpenGL | Vulkan |
|---|---|---|
| Scene intersection | Depth snapshot per view (`CopyFramebufferDepthToTexture`). The ray ends exactly at the scene, so there is no hard line where the beam meets a hill. | None, because the frame pass does not store depth. From outside, the front faces are depth-tested. From inside, the back faces draw untested. The axis raycast ends the cone softly at terrain. |
| Light pool | Deferred box decal: it relights the true scene points, and the normal comes from depth derivatives. | A quad in the plane of the face that was hit. |
| Glare | Occlusion from a 5-tap depth probe. Drawn untested so the streak spills over geometry. | Depth-tested sprite. |

`OBEY_BEAM_DEPTH=0` disables the GL snapshot so you can A/B it against the Vulkan path on GL.
With a shader pack active (an offscreen target bound) the snapshot is skipped automatically.

## Limits

- Translucent terrain is drawn after the block entities, so water or glass *in front of* a
  beam dims it by its alpha. That is the same order as MC's beacon beam.
- On Vulkan the light pool is flat. Where the terrain steps away from the plane of the hit
  face, the splash floats slightly or sinks.
- Shaders: `shaders/beam_volume.{vert,frag}`, `beam_sprite.vert` (glare and pool quad),
  `beam_glare.frag` and `beam_pool.frag`, each with a `_vk` twin listed in `VK_SHADERS`
  (recompile the `.spv` with glslc). The GL and Vulkan files share their function bodies
  verbatim; only the declarations differ, so edit both. The Vulkan twins read the per-cone
  parameters from the Common UBO under the portal renderer's names (`uPortalColor`,
  `uColorDark`, `uColorHot`, `uKeyDir`, `uScalarsA`/`B`, `uTint`). The slot table is at
  the top of `VolumetricBeam.cpp`.
