// File: shaders/terrain_solid.frag
// No-discard TERRAIN fragment shader: block_solid.frag plus the
// greedy-meshing tile-rect sample path. Used for the translucent pass
// (water, ice, stained glass) where blending handles transparency. NOTE the
// mesher never merges translucent quads (sort granularity), so the tiled
// branch is only ever taken here if that policy changes — it is kept so all
// three terrain fragment shaders share one vertex format and one contract.
// Keep in step with block_solid.frag apart from the sampling.
#version 330 core

in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;
flat in int fragSprite;   // see terrain.vert; -1 = untiled
flat in int fragRecord;   // face-mapped: first record texel
flat in int fragAux;      // two-sided: back mapping

uniform sampler2D uTextureAtlas;
// Atlas sprite table (AtlasBuilder), texture unit 1: texel (id & 255,
// id >> 8) = (u0, v0, width, height) of sprite `id` in atlas UV.
uniform sampler2D uSpriteTable;
// Face map, texture unit 2: a buffer texture over the slab vertex buffer
// holding the per-block records of face-mapped rectangles (Vertex.hpp).
uniform samplerBuffer uFaceMap;
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1)
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off
// Debug fill override (greedy-mesh view): rgb painted at strength a over the
// final color. Zero (the GL default for an unset uniform) = passthrough.
uniform vec4 uOverlayColor;

out vec4 FragColor;

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
        if (dot(n, uCameraPos - fragWorldPos) < 0.0) {
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
    vec3 finalColor = textureColor.rgb * vcol.rgb;

    // Day/night sky-light dim + MC-style distance fog
    finalColor *= uSkyBrightness;
    vec3 fogDelta = fragWorldPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    finalColor = mix(finalColor, uFogColor.rgb, fogValue * uFogColor.a);

    finalColor = mix(finalColor, uOverlayColor.rgb, uOverlayColor.a);
    FragColor = vec4(finalColor, textureColor.a * vcol.a);
}
