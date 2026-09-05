// File: shaders/terrain_opaque_vk.frag (Vulkan twin of terrain_opaque.frag)
// Opaque (solid-layer) TERRAIN fragment shader: block_opaque_vk.frag plus the
// greedy-meshing tile-rect sample path. NO discard, NO alpha test. The
// mesher-enforced no-alpha-dependence invariant from block_opaque_vk.frag
// holds unchanged. Keep in step with the GL terrain_opaque.frag.
#version 450

// Input from vertex shader
layout (location = 0) in vec2 fragTexCoord;
layout (location = 1) in vec3 fragWorldPos;
layout (location = 2) in vec4 fragColor;
layout (location = 3) flat in int fragSprite;  // see terrain_vk.vert; -1 = untiled
layout (location = 4) flat in int fragRecord;  // face-mapped: first record texel
layout (location = 5) flat in int fragAux;     // two-sided: back mapping

// Texture atlas sampler (descriptor set 0, binding 0)
layout (set = 0, binding = 0) uniform sampler2D uTextureAtlas;
// Atlas sprite table (AtlasBuilder), texture slot 1 = descriptor set 2:
// texel (id & 255, id >> 8) = (u0, v0, width, height) of sprite `id`.
layout (set = 2, binding = 0) uniform sampler2D uSpriteTable;
// Face map (descriptor set 4): a buffer texture over the slab vertex buffer
// holding the per-block records of face-mapped rectangles (Vertex.hpp).
layout (set = 4, binding = 0) uniform samplerBuffer uFaceMap;

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
    vec2  _pad_;
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

// A face-mapped rectangle's per-block record, one RGBA16 texel: tint * face
// shade (three bytes), the four 2-bit AO corner codes (a byte) and the
// sprite id (16 bits). The block is found from the tile-space uv: floor(uv)
// minus the rectangle's tile origin, row-major by the rectangle's width.
// Clamped so an edge pixel whose interpolated uv lands exactly on the far
// boundary reads its own rectangle's last record, never a neighbour's.
struct FaceRecord { vec3 color; int aoCodes; int sprite; };
FaceRecord fetchFaceRecord(vec2 uv) {
    int w   = fragSprite & 0x1F;
    int h   = (fragSprite >> 5) & 0x1F;
    int tv0 = (fragSprite >> 10) & 0xF;
    int tu0 = (fragSprite >> 14) & 0xF;
    ivec2 cell = clamp(ivec2(floor(uv)) - ivec2(tu0, tv0), ivec2(0), ivec2(w - 1, h - 1));
    // One RGBA16 texel per record: r = colour r | g << 8, g = colour b |
    // AO codes << 8, b = sprite id (unorm16 -> integer is exact in fp32).
    ivec4 t = ivec4(texelFetch(uFaceMap, fragRecord + cell.x + cell.y * w) * 65535.0 + 0.5);
    FaceRecord r;
    r.color   = vec3(float(t.r & 0xFF), float(t.r >> 8), float(t.g & 0xFF)) / 255.0;
    r.aoCodes = t.g >> 8;
    r.sprite  = t.b;
    return r;
}

// Greedy-merged quads tile their sprite: uv is in tile space (0..N block
// repeats) and the sprite's atlas rect is fetched from the sprite table by
// id — the vertex's id for a fluid plate, the block's own record for a
// face-mapped rectangle. fract() folds each repeat back onto the sprite;
// textureGrad with the UNWRAPPED uv's derivatives (scaled into atlas space)
// keeps mip selection identical to an unmerged quad — fract()'s sawtooth
// would otherwise spike dFdx at every block seam and drop those pixel
// columns onto the smallest mip. Unmerged quads (fragSprite < 0) take the
// plain texture() path so their output stays bit-identical to the
// pre-greedy shader. Derivatives are computed before the branch.
vec4 sampleTerrainAtlas(int sprite, vec2 uv) {
    vec2 texDx = dFdx(uv);
    vec2 texDy = dFdy(uv);
    if (fragSprite >= 0) {
        // (u0, v0, width, height) of the sprite in atlas UV — 256 per row.
        vec4 rect = texelFetch(uSpriteTable, ivec2(sprite & 255, sprite >> 8), 0);
        vec2 atlasUV = rect.xy + fract(uv) * rect.zw;
        return textureGrad(uTextureAtlas, atlasUV, texDx * rect.zw, texDy * rect.zw);
    }
    return texture(uTextureAtlas, uv);
}

// Vertex colour with ambient occlusion re-applied per block. A face-mapped
// rectangle's record carries tint * face shade and the four corner AO levels
// of THAT block's face (2 bits each, 0..3 = 1.0, 0.8, 0.6, 0.4; bits 0-1 =
// tile corner (0,0), 2-3 = (1,0), 4-5 = (0,1), 6-7 = (1,1)). The gradient is
// rebuilt from the fractional tile coordinate with exactly the interpolation
// an unmerged quad gets from the GPU: two triangles split on the (1,0)-(0,1)
// diagonal (the mesher's corner order puts vertices 0 and 2 there for every
// face), linear inside each. Unmerged quads and fluid plates keep their
// colour baked in the vertex and take the plain path.
vec4 shadedVertexColor(bool mapped, FaceRecord rec, vec2 uv) {
    vec4 c = fragColor;
    // A two-sided quad's alpha byte is its mirror sum, not a tint alpha.
    if (fragSprite >= 0 && (fragSprite & 0x20000) != 0) c.a = 1.0;
    if (mapped) {
        int codes = rec.aoCodes;
        float a00 = 1.0 - 0.2 * float( codes       & 3);
        float a10 = 1.0 - 0.2 * float((codes >> 2) & 3);
        float a01 = 1.0 - 0.2 * float((codes >> 4) & 3);
        float a11 = 1.0 - 0.2 * float((codes >> 6) & 3);
        vec2 f = fract(uv);
        float ao = (f.x + f.y <= 1.0)
            ? a00 + (a10 - a00) * f.x + (a01 - a00) * f.y
            : a11 + (a10 - a11) * (1.0 - f.y) + (a01 - a11) * (1.0 - f.x);
        c.rgb = rec.color * ao;
        c.a = 1.0;
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
        int code = (fragSprite >> 19) & 0x3F;
        vec3 n = vec3(float((code & 3) == 1) - float((code & 3) == 3),
                      float(((code >> 2) & 3) == 1) - float(((code >> 2) & 3) == 3),
                      float(((code >> 4) & 3) == 1) - float(((code >> 4) & 3) == 3));
        if (dot(n, U.uCamPosBright_.xyz - fragWorldPos) < 0.0) {
            // fragAux: bits 0..5 = mirrored extent sum in sixteenths, bits
            // 6..7 = mode (0 mirror u, 1 mirror v, 2 identical mapping).
            float sum = float(fragAux & 0x3F) / 16.0;
            int mode = (fragAux >> 6) & 3;
            if (mode == 0) uv.x = sum - uv.x;
            else if (mode == 1) uv.y = sum - uv.y;
        }
    }
    FaceRecord rec = FaceRecord(vec3(1.0), 0, 0);
    if (mapped) rec = fetchFaceRecord(uv);
    vec4 textureColor = sampleTerrainAtlas(mapped ? rec.sprite : (fragSprite & 0xFFFF), uv);
    vec4 vcol = shadedVertexColor(mapped, rec, uv);

    // Vertex color contains: biome tint * AO * directional face shade
    vec3 finalColor = textureColor.rgb * vcol.rgb;

    // Day/night sky-light dim + MC-style distance fog
    finalColor *= U.uCamPosBright_.w;
    vec3 fogDelta = fragWorldPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    finalColor = mix(finalColor, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);

    finalColor = mix(finalColor, U.uOverlayColor_.rgb, U.uOverlayColor_.a);
    FragColor = vec4(finalColor, textureColor.a * vcol.a);
}
