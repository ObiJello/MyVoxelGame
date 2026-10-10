// File: shaders/terrain_solid.frag
// Translucent-pass TERRAIN fragment shader: block_solid.frag plus the
// greedy-meshing tile-rect sample path and MC's translucent cutout. Used for
// the translucent pass (water, glass, ice, stained glass) where blending
// handles transparency. It still discards (MC 26.3 TRANSLUCENT_TERRAIN,
// ALPHA_CUTOUT 0.1): the pass writes depth, and a fully clear texel that
// is not discarded — glass's interior — would write depth over everything
// drawn after it, so water behind a pane vanished whenever the pane's quad
// sorted first. NOTE the
// mesher never merges translucent quads (sort granularity), so the tiled
// branch is only ever taken here if that policy changes — it is kept so all
// three terrain fragment shaders share one vertex format and one contract.
// Keep in step with block_solid.frag apart from the sampling.
#version 330 core

// Half precision (TERRAIN_F16 — the <name>_f16_vk.frag.spv variant CMake
// compiles, which Metal and a Vulkan device with shaderFloat16 load): the
// colour pipeline — the atlas texel, lightmap colours, AO, tint, the fog
// mixes — in float16; texture coordinates, fog distances and the record's
// integer fields stay 32-bit. On Apple GPUs half runs at twice the rate of
// float and halves the colour pipeline's register footprint (occupancy).
// Without the define every h* type is its 32-bit twin, so the GL twin and
// the float .spv read the same source.
#ifdef TERRAIN_F16
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require   // the half varyings (terrain_vk.vert)
#define hfloat float16_t
#define hvec2 f16vec2
#define hvec3 f16vec3
#define hvec4 f16vec4
#else
#define hfloat float
#define hvec2 vec2
#define hvec3 vec3
#define hvec4 vec4
#endif

in vec2 fragTexCoord;
in vec2 fragFog;   // (spherical, cylindrical) fog distance, per vertex (terrain.vert)
in vec4 fragColor;
flat in int fragSprite;   // see terrain.vert; -1 = untiled
flat in int fragRecord;   // face-mapped: first record texel
flat in int fragAux;      // two-sided: back mapping

uniform sampler2D uTextureAtlas;
// Atlas sprite table (AtlasBuilder), texture unit 1: texel (id & 255,
// id >> 8) = (u0, v0, width, height) of sprite `id` in atlas UV.
uniform sampler2D uSpriteTable;
// The block atlas's sprite array (AtlasBuilder::BuildSpriteArray), texture
// unit 4: layer `id` is static sprite `id`, for ids below uSpriteLayers —
// see sampleTerrainAtlas. 0 = no array.
uniform sampler2DArray uSpriteArray;
uniform sampler2DArray uSpriteArray2;   // unit 5: the overflow past uSpriteArrayCap layers
uniform int uSpriteLayers;
uniform int uSpriteArrayCap;            // layers per array: id s is layer s % cap of array s / cap
// Face map, texture unit 2: a buffer texture over the slab vertex buffer
// holding the per-block records of face-mapped rectangles (Vertex.hpp).
uniform usamplerBuffer uFaceMap;   // RGBA32UI texels: one 16-byte record each (block or quad record)
// MC's lightmap (texture unit 3) — the vertex shader's, read here for
// face-mapped rectangles, whose light is per block (the record's second
// texel).
uniform sampler2D uLightmap;
uniform int uWorldLighting;
uniform vec4 uTerrainLight;   // x = light mode (ChunkRenderer::SetEnvironmentUniforms), see shadedVertexColor   // 0: World Lighting off — the lightmap is white, so it is not sampled
// MC sample_lightmap.glsl: uv are the light coords (0..240 each). MC's clamp
// is dropped: every light word the mesher writes is a MC light coord
// (TerrainVertex::LightWord, 0..240 per axis), so uv / 256 + 1/32 is inside
// [1/32, 31/32] by construction — one fma per axis instead of six ops, times
// four samples per fragment.
hvec3 sampleLightmap(vec2 uv) {
    return hvec3(texture(uLightmap, uv * (1.0 / 256.0) + (0.5 / 16.0)).rgb);
}
// MC ALPHA_CUTOUT for the pass (ChunkRenderer::RenderLayerPass: 0.1, MC 26.3
// RenderPipelines.TRANSLUCENT_TERRAIN), tested on the final alpha.
uniform float uAlphaTest;
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off
// Debug fill override (greedy-mesh view): rgb painted at strength a over the
// final color. Zero (the GL default for an unset uniform) = passthrough.
uniform vec4 uOverlayColor;

#ifndef OIT_ALPHA_ONLY
out vec4 FragColor;
#endif

// Improved Transparency (MC 26.3 OIT, Render::ImprovedTransparency): the
// OIT variants of this shader are its source with `#define OIT` and a stage
// define; the backend splices shaders/oit_lib.glsl in here. The engine's own
// compile never sees any of it.
#ifdef OIT
#pragma oit_library
#endif

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

// A face-mapped rectangle's per-block record: ONE RGBA32UI texel (the
// slab's face map, 16 bytes a record, the same view the quad records live
// in — Vertex.hpp). x = tint * face shade (three bytes) with the four 2-bit
// AO corner codes in the top byte; y = the sprite id; z, w = the block
// face's four corner light words (MC light coords, block8 | sky8 << 8) in
// tile-corner order — (0,0) | (1,0) << 16, (0,1) | (1,1) << 16 — so light
// varies per block like AO and does not stop faces from merging. The block
// is found from the tile-space uv: floor(uv) minus the rectangle's tile
// origin, row-major by the rectangle's width. Clamped so an edge pixel whose
// interpolated uv lands exactly on the far boundary reads its own
// rectangle's last record, never a neighbour's.
struct FaceRecord { hvec3 color; int aoCodes; int sprite; uvec2 lights; };
FaceRecord fetchFaceRecord(vec2 uv) {
    int w   = fragSprite & 0x1F;
    int h   = (fragSprite >> 5) & 0x1F;
    int tv0 = (fragSprite >> 10) & 0xF;
    int tu0 = (fragSprite >> 14) & 0xF;
    ivec2 cell = clamp(ivec2(floor(uv)) - ivec2(tu0, tv0), ivec2(0), ivec2(w - 1, h - 1));
    uvec4 t = texelFetch(uFaceMap, fragRecord + cell.x + cell.y * w);
    FaceRecord r;
    r.color   = hvec3(ivec3(int(t.x & 0xFFu), int((t.x >> 8) & 0xFFu), int((t.x >> 16) & 0xFFu))) * hfloat(1.0 / 255.0);
    r.aoCodes = int(t.x >> 24);
    r.sprite  = int(t.y & 0xFFFFu);
    r.lights  = t.zw;
    return r;
}

// Greedy-merged quads tile their sprite: uv is in tile space (0..N block
// repeats); the vertex's id for a fluid plate, the block's own record for a
// face-mapped rectangle names the sprite. A STATIC sprite is a layer of the
// sprite arrays (AtlasBuilder::BuildSpriteArray: the ids below uSpriteLayers,
// id s = layer s % cap of array s / cap), sampled with hardware repeat and the hardware's own
// mip selection — a layer is the sprite's own mip chain, upscaled to the
// array's size by a whole power of two, so the level the GPU picks holds
// exactly the atlas texels the atlas path fetched. Animated sprites (drawn
// into the atlas every tick) and sprites that do not fit the array keep the
// atlas: fract() folds each repeat back onto the sprite's rect from the
// sprite table, and textureGrad with the UNWRAPPED uv's derivatives (scaled
// into atlas space) keeps mip selection identical to an unmerged quad —
// fract()'s sawtooth would otherwise spike dFdx at every block seam and
// drop those pixel columns onto the smallest mip. Unmerged quads
// (fragSprite < 0) take the plain texture() path so their output stays
// bit-identical to the pre-greedy shader. Derivatives are computed before
// the branches (the first is flat, the second decided per block).
hvec4 sampleTerrainAtlas(int sprite, vec2 uv) {
    vec2 texDx = dFdx(uv);
    vec2 texDy = dFdy(uv);
    if (fragSprite >= 0) {
        if (sprite < uSpriteLayers) {
            if (sprite < uSpriteArrayCap) return hvec4(texture(uSpriteArray, vec3(uv, float(sprite))));
            return hvec4(texture(uSpriteArray2, vec3(uv, float(sprite - uSpriteArrayCap))));
        }
        // (u0, v0, width, height) of the sprite in atlas UV — 256 per row.
        vec4 rect = texelFetch(uSpriteTable, ivec2(sprite & 255, sprite >> 8), 0);
        vec2 atlasUV = rect.xy + fract(uv) * rect.zw;
        return hvec4(textureGrad(uTextureAtlas, atlasUV, texDx * rect.zw, texDy * rect.zw));
    }
    return hvec4(texture(uTextureAtlas, uv));
}
// Vertex colour with ambient occlusion re-applied per block. A face-mapped
// rectangle's record carries tint * face shade and the four corner AO levels
// of THAT block's face (2 bits each, 0..3 = 1.0, 0.8, 0.6, 0.4; bits 0-1 =
// tile corner (0,0), 2-3 = (1,0), 4-5 = (0,1), 6-7 = (1,1)). The gradient is
// rebuilt — with the block's four corner lights — from the fractional tile
// coordinate with exactly the interpolation
// an unmerged quad gets from the GPU: two triangles split on the (1,0)-(0,1)
// diagonal (the mesher's corner order puts vertices 0 and 2 there for every
// face), linear inside each. Unmerged quads and fluid plates keep their
// colour baked in the vertex and take the plain path.
hvec4 shadedVertexColor(bool mapped, FaceRecord rec, vec2 uv) {
    hvec4 c = hvec4(fragColor);
    // A two-sided quad's alpha byte is its mirror sum, not a tint alpha.
    if (fragSprite >= 0 && (fragSprite & 0x20000) != 0) c.a = hfloat(1.0);
    if (mapped) {
        int codes = rec.aoCodes;
        hfloat a00 = hfloat(1.0) - hfloat(0.2) * hfloat( codes       & 3);
        hfloat a10 = hfloat(1.0) - hfloat(0.2) * hfloat((codes >> 2) & 3);
        hfloat a01 = hfloat(1.0) - hfloat(0.2) * hfloat((codes >> 4) & 3);
        hfloat a11 = hfloat(1.0) - hfloat(0.2) * hfloat((codes >> 6) & 3);
        // The GPU's interpolation of an unmerged quad's vertex colours: two
        // triangles split on the (1,0)-(0,1) diagonal, linear inside each.
        vec2 f = fract(uv);
        bool lower = f.x + f.y <= 1.0;
        hfloat fx = hfloat(f.x), fy = hfloat(f.y);
        // Each corner's colour is AO times the lightmap at its light — what
        // an unmerged quad's vertices carry (MC terrain.vsh: vertexColor =
        // Color * sample_lightmap(UV2)) — and the corners blend exactly as
        // the GPU blends those vertex colours. (One sample for a rectangle
        // whose corners share a light, per fragment or per primitive, was
        // measured a loss on 2026-10-08: divergence, then registers.)
        // World Lighting off (Lightmap::WorldLightingOn, uWorldLighting): the
        // lightmap is white, so the light texel and the four samples are
        // skipped — AO alone shades the corners (a uniform branch).
        hvec3 lit;
        if (uWorldLighting != 0) {
            ivec4 L = ivec4(int(rec.lights.x & 0xFFFFu), int(rec.lights.x >> 16),
                            int(rec.lights.y & 0xFFFFu), int(rec.lights.y >> 16));   // the block's four corner lights
            vec2 l00 = vec2(float(L.x & 0xFF), float(L.x >> 8)), l10 = vec2(float(L.y & 0xFF), float(L.y >> 8));
            vec2 l01 = vec2(float(L.z & 0xFF), float(L.z >> 8)), l11 = vec2(float(L.w & 0xFF), float(L.w >> 8));
            int lightMode = int(uTerrainLight.x);   // OBEY_LIGHT_MODE (ChunkRenderer::SetEnvironmentUniforms); uniform
            if (lightMode == 0) {
                hvec3 c00 = a00 * sampleLightmap(l00);
                hvec3 c10 = a10 * sampleLightmap(l10);
                hvec3 c01 = a01 * sampleLightmap(l01);
                hvec3 c11 = a11 * sampleLightmap(l11);
                lit = lower ? c00 + (c10 - c00) * fx + (c01 - c00) * fy
                                  : c11 + (c10 - c11) * (hfloat(1.0) - fy) + (c01 - c11) * (hfloat(1.0) - fx);
            } else {
                // Mode 1 (the default): the corner LIGHT COORDS blend over
                // the same two triangles and the lightmap is sampled once at
                // the blend — not MC's blend of four colours: the lightmap
                // curve bends between the corners' levels (exact at them).
                // Still frame 2026-10-09: +2.0 % fps, GPU frame -6 %.
                vec2 lc = lower ? l00 + (l10 - l00) * f.x + (l01 - l00) * f.y
                                : l11 + (l10 - l11) * (1.0 - f.y) + (l01 - l11) * (1.0 - f.x);
                hfloat ao = lower ? a00 + (a10 - a00) * fx + (a01 - a00) * fy
                                  : a11 + (a10 - a11) * (hfloat(1.0) - fy) + (a01 - a11) * (hfloat(1.0) - fx);
                lit = ao * sampleLightmap(lc);
            }
        } else {
            lit = hvec3(lower ? a00 + (a10 - a00) * fx + (a01 - a00) * fy
                              : a11 + (a10 - a11) * (hfloat(1.0) - fy) + (a01 - a11) * (hfloat(1.0) - fx));
        }
        c.rgb = rec.color * lit;
        c.a = hfloat(1.0);
    }
    return c;
}

void main() {

    // Face-mapped rectangles read the block's own record; the branch is
    // uniform per primitive (fragSprite is flat).
    bool mapped = fragSprite >= 0 && (fragSprite & 0x40000) != 0;
    // A two-sided quad seen from its geometric back is the thin element's
    // OTHER face, whose uv runs the opposite way along the quad: mirror u
    // within the sprite (tile space, 0..1 for a single block). The side is
    // the camera's position against the quad's stored front normal (two
    // bits per axis: 1 = +, 3 = -), which is exact for the axis-aligned and
    // 45-degree planes the mesher admits and holds in mirrored portal views
    // where winding-based front facing would not. Constant per primitive,
    // so no divergence and the derivatives only change sign.
    vec2 uv = fragTexCoord;
    if (fragSprite >= 0 && (fragSprite & 0x20000) != 0) {
        // Bit 8 of fragAux: the camera is behind the quad — one sign over
        // the quad's plane, so decided per vertex (the vertex shader).
        if ((fragAux & 0x100) != 0) {
            // fragAux: bits 0..5 = mirrored extent sum in sixteenths, bits
            // 6..7 = mode (0 mirror u, 1 mirror v, 2 identical mapping).
            float sum = float(fragAux & 0x3F) / 16.0;
            int mode = (fragAux >> 6) & 3;
            if (mode == 0) uv.x = sum - uv.x;
            else if (mode == 1) uv.y = sum - uv.y;
        }
    }
    FaceRecord rec = FaceRecord(hvec3(1.0), 0, 0, uvec2(0u));
    if (mapped) rec = fetchFaceRecord(uv);
    hvec4 textureColor = sampleTerrainAtlas(mapped ? rec.sprite : (fragSprite & 0xFFFF), uv);
    // MC terrain.fsh: `if (color.a < ALPHA_CUTOUT) discard;` on texture x
    // vertex colour. A discarded texel writes no depth, which is what keeps
    // an invisible pane interior from hiding the water behind it. The
    // vertex alpha is what shadedVertexColor returns in .a (1 for a mapped
    // or two-sided quad, the tint alpha otherwise), known before the
    // lighting, so the test runs first and a discarded texel skips it.
    hfloat vertexAlpha = (mapped || (fragSprite >= 0 && (fragSprite & 0x20000) != 0)) ? hfloat(1.0) : hfloat(fragColor.a);
    float alpha = float(textureColor.a * vertexAlpha);
    if (alpha < uAlphaTest) {
        discard;
    }
    hvec4 vcol = shadedVertexColor(mapped, rec, uv);
#ifdef OIT_ALPHA_ONLY
    // MC terrain.fsh: the depth-bounds / transmittance stages read alpha only.
    executeAlphaOnlyPhase(gl_FragCoord.z, alpha);
#else
    hvec3 finalColor = textureColor.rgb * vcol.rgb;

    // MC fog.glsl apply_fog over the vertex shader's interpolated distances.
    // Fog off (Video Settings): uFogColor.a is 0 and the distances are out
    // of reach anyway — skip the two ramps and the mix (a uniform branch).
    if (uFogColor.a > 0.0) {
        float fogValue = max(linearFog(fragFog.x, uFogEnv.x, uFogEnv.y),
                             linearFog(fragFog.y, uFogEnv.z, uFogEnv.w));
        finalColor = mix(finalColor, hvec3(uFogColor.rgb), hfloat(fogValue * uFogColor.a));
    }
    // MC terrain.fsh: a freshly loaded section fades in from the fog
    // colour — mix(FogColor, color, ChunkVisibility), alpha untouched.
    finalColor = mix(hvec3(uFogColor.rgb), finalColor, hfloat(float((fragAux >> 16) & 0xFF) * (1.0 / 255.0)));   // ChunkVisibility, terrain.vert

    finalColor = mix(finalColor, hvec3(uOverlayColor.rgb), hfloat(uOverlayColor.a));
    FragColor = vec4(vec3(finalColor), alpha);
#ifdef OIT_ACCUMULATE
    // MC calculateFinalColor: premultiplied, weighted by the transmittance
    // in front of it (the fog is already in the colour).
    FragColor = sampleColorForAccumulation(FragColor);
#endif
#endif
}
