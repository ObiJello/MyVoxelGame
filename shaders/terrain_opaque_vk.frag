// File: shaders/terrain_opaque_vk.frag (Vulkan twin of terrain_opaque.frag)
// Opaque (solid-layer) TERRAIN fragment shader: block_opaque_vk.frag plus the
// greedy-meshing tile-rect sample path. NO discard, NO alpha test. The
// mesher-enforced no-alpha-dependence invariant from block_opaque_vk.frag
// holds unchanged. Keep in step with the GL terrain_opaque.frag.
#version 450

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

// Input from vertex shader
layout (location = 0) in vec2 fragTexCoord;
layout (location = 1) in hvec2 fragFog;  // (spherical, cylindrical) fog distance, per vertex (terrain_vk.vert)
layout (location = 2) in hvec4 fragColor;
layout (location = 3) flat in int fragSprite;  // see terrain_vk.vert; -1 = untiled
layout (location = 4) flat in int fragRecord;  // face-mapped: first record texel
layout (location = 5) flat in int fragAux;     // two-sided: back mapping

// Texture atlas sampler (descriptor set 0, binding 0)
layout (set = 0, binding = 0) uniform sampler2D uTextureAtlas;
// Atlas sprite table (AtlasBuilder), texture slot 1 = descriptor set 2:
// texel (id & 255, id >> 8) = (u0, v0, width, height) of sprite `id`.
layout (set = 2, binding = 0) uniform sampler2D uSpriteTable;
// The block atlas's sprite array (AtlasBuilder::BuildSpriteArray), texture
// slot 4 = descriptor set 7: layer `id` is static sprite `id`, for ids below
// uSpriteLayers (U.uSpriteArray_.x); id s is layer s % cap (U.uSpriteArray_.y)
// of array s / cap — see sampleTerrainAtlas.
layout (set = 7, binding = 0) uniform sampler2DArray uSpriteArray;
layout (set = 7, binding = 1) uniform sampler2DArray uSpriteArray2;   // the overflow past uSpriteArrayCap layers
// Face map (descriptor set 4): a buffer texture over the slab vertex buffer
// holding the per-block records of face-mapped rectangles (Vertex.hpp).
layout (set = 4, binding = 0) uniform usamplerBuffer uFaceMap;   // RGBA16UI texels: integer fields, no unorm round trip
// MC's lightmap (Render::Lightmap), texture slot 3 = descriptor set 5 —
// the vertex shader's, read here for face-mapped rectangles, whose light
// is per block (the record's second texel).
layout (set = 5, binding = 0) uniform sampler2D uLightmap;
// MC sample_lightmap.glsl: uv are the light coords (0..240 each). MC's clamp
// is dropped: every light word the mesher writes is a MC light coord
// (TerrainVertex::LightWord, 0..240 per axis), so uv / 256 + 1/32 is inside
// [1/32, 31/32] by construction — one fma per axis instead of six ops, times
// four samples per fragment.
hvec3 sampleLightmap(vec2 uv) {
    return hvec3(texture(uLightmap, uv * (1.0 / 256.0) + (0.5 / 16.0)).rgb);
}

// Push constants (must match C++ PushConstantBlock layout exactly)
layout (push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

// Common UBO (portal pipeline layout, set=1) — see block_vk.frag.
layout (std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;
    vec4  uPortalColor_;
    vec4  uColorDark_;
    vec4  uColorHot_;
    vec4  uKeyDir_;
    vec4  uTint_;
    vec4  uUVRange_;
    vec4  uScalarsA_;
    vec4  uScalarsB_;
    vec4  uScalarsC_;
    vec4  uScalarsD_;
    vec2  uScreenSize_;
    vec2  uSpriteArray_;   // (sprite ids in the sprite arrays, layers per array) — SpirvUniforms.hpp
    vec4  uFogColor_;
    vec4  uFogEnv_;
    vec4  uCamPosBright_;
    vec4  uOverlayColor_;  // debug fill override (a=0 normally)
} U;

// Output
layout (location = 0) out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

// A face-mapped rectangle's per-block record, two RGBA16 texels. The first:
// tint * face shade (three bytes), the four 2-bit AO corner codes (a byte)
// and the sprite id (16 bits). The second: the block face's four corner
// light words (MC light coords, block8 | sky8 << 8) in tile-corner order —
// (0,0), (1,0), (0,1), (1,1) — so light varies per block like AO and does
// not stop faces from merging. The block is found from the tile-space uv:
// floor(uv) minus the rectangle's tile origin, row-major by the rectangle's
// width. Clamped so an edge pixel whose interpolated uv lands exactly on the
// far boundary reads its own rectangle's last record, never a neighbour's.
struct FaceRecord { hvec3 color; int aoCodes; int sprite; int texel; };
FaceRecord fetchFaceRecord(vec2 uv) {
    int w   = fragSprite & 0x1F;
    int h   = (fragSprite >> 5) & 0x1F;
    int tv0 = (fragSprite >> 10) & 0xF;
    int tu0 = (fragSprite >> 14) & 0xF;
    ivec2 cell = clamp(ivec2(floor(uv)) - ivec2(tu0, tv0), ivec2(0), ivec2(w - 1, h - 1));
    int texel = fragRecord + 2 * (cell.x + cell.y * w);
    // r = colour r | g << 8, g = colour b | AO codes << 8, b = sprite id
    // (unorm16 -> integer is exact in fp32).
    ivec4 t = ivec4(texelFetch(uFaceMap, texel));
    FaceRecord r;
    r.color   = hvec3(ivec3(t.r & 0xFF, t.r >> 8, t.g & 0xFF)) * hfloat(1.0 / 255.0);
    r.aoCodes = t.g >> 8;
    r.sprite  = t.b;
    // The light texel (texel + 1) is read in shadedVertexColor, after the
    // alpha test of the cutout and translucent shaders: a discarded
    // fragment (most texels of a cross plant) never fetches it nor samples
    // the lightmap four times. (Reading it here, with the record, in the
    // opaque shader measured −1..−2.5 % live, 2026-10-08: the compiler
    // schedules the reads itself.)
    r.texel   = texel;
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
        if (sprite < int(U.uSpriteArray_.x)) {
            int cap = int(U.uSpriteArray_.y);
            if (sprite < cap) return hvec4(texture(uSpriteArray, vec3(uv, float(sprite))));
            return hvec4(texture(uSpriteArray2, vec3(uv, float(sprite - cap))));
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
        if (U.uScalarsD_.w > 0.5) {
            ivec4 L = ivec4(texelFetch(uFaceMap, rec.texel + 1));   // the block's four corner lights
            hvec3 c00 = a00 * sampleLightmap(vec2(float(L.x & 0xFF), float(L.x >> 8)));
            hvec3 c10 = a10 * sampleLightmap(vec2(float(L.y & 0xFF), float(L.y >> 8)));
            hvec3 c01 = a01 * sampleLightmap(vec2(float(L.z & 0xFF), float(L.z >> 8)));
            hvec3 c11 = a11 * sampleLightmap(vec2(float(L.w & 0xFF), float(L.w >> 8)));
            lit = lower ? c00 + (c10 - c00) * fx + (c01 - c00) * fy
                              : c11 + (c10 - c11) * (hfloat(1.0) - fy) + (c01 - c11) * (hfloat(1.0) - fx);
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
    FaceRecord rec = FaceRecord(hvec3(1.0), 0, 0, 0);
    if (mapped) rec = fetchFaceRecord(uv);
    hvec4 textureColor = sampleTerrainAtlas(mapped ? rec.sprite : (fragSprite & 0xFFFF), uv);
    hvec4 vcol = shadedVertexColor(mapped, rec, uv);

    // Vertex color contains: biome tint * AO * directional face shade
    hvec3 finalColor = textureColor.rgb * vcol.rgb;

    // MC fog.glsl apply_fog over the vertex shader's interpolated distances.
    // Fog off (Video Settings): uFogColor.a is 0 and the distances are out
    // of reach anyway — skip the two ramps and the mix (a uniform branch).
    if (U.uFogColor_.a > 0.0) {
        float fogValue = max(linearFog(float(fragFog.x), U.uFogEnv_.x, U.uFogEnv_.y),
                             linearFog(float(fragFog.y), U.uFogEnv_.z, U.uFogEnv_.w));
        finalColor = mix(finalColor, hvec3(U.uFogColor_.rgb), hfloat(fogValue * U.uFogColor_.a));
    }
    // MC terrain.fsh: a freshly loaded section fades in from the fog
    // colour — mix(FogColor, color, ChunkVisibility), alpha untouched.
    finalColor = mix(hvec3(U.uFogColor_.rgb), finalColor, hfloat(float((fragAux >> 16) & 0xFF) * (1.0 / 255.0)));   // ChunkVisibility, terrain_vk.vert

    finalColor = mix(finalColor, hvec3(U.uOverlayColor_.rgb), hfloat(U.uOverlayColor_.a));
    FragColor = vec4(vec3(finalColor), float(textureColor.a * vcol.a));
}
