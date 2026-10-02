// File: shaders/block_instanced.vert
#version 330 core

// The instanced twin of block.vert, for BlockCubeEntityRenderer.
//
// Same geometry, same fragment shader (block.frag, unchanged), one difference:
// the model transform arrives as ONE per-instance vec4 attribute — xyz the
// world translation, w a uniform scale — instead of being folded into a uMVP
// uniform on the CPU. That is the whole point — a hundred thousand primed TNT
// were a hundred thousand glDrawElements calls at Apple's ~1 us/sub-draw
// floor, and this collapses each block state into one glDrawElementsInstanced.
//
// Translate + uniform scale is the ENTIRE transform space of the two block
// entities: MC's FallingBlockRenderer is a pure translate, and TntRenderer's
// lift/swell/centre chain collapses to a scale about the cube's centre plus a
// translate (BlockCubeEntityRenderer derives it). A full mat4 per instance
// carried 64 bytes for 16 bytes of information, four times the upload.
//
// The overlay colour is deliberately NOT per-instance. TNT's white flash has
// exactly two states, so the renderer groups by it and keeps block.frag's
// uOverlayColor uniform — which is what lets this share that shader instead of
// forking it.
//
// NOTE the world-position difference from block.vert. There, aPos is already
// world-space (the chunk mesher emits it that way) and fragWorldPos is aPos
// verbatim. Here aPos is MODEL-space, so both fragWorldPos and the portal clip
// distance must use the transformed position — using aPos would fog and clip
// every instance as if it stood at the origin.
//
// ── Light: per vertex, from the instance's 3x3x3 cell light ───────────────
//
// MC draws a falling block through MovingBlockFeatureRenderer:
// ModelBlockRenderer.tesselateBlock with the MovingBlockRenderState as the
// level — its own state at its blockPos, AIR everywhere else, the client
// level's light engine for every brightness. So each vertex's light is MC
// BlockModelLighter's (smooth: the four-corner smoothBlend around the
// vertex, weighted by the quad's shape; flat: one cell per quad) over the
// level's light around blockPos, which moves with the block every tick.
//
// The cells' light arrives per instance (aCells*: the MC packed light,
// block | sky << 4, of the 27 cells around blockPos, index
// (dx + 1) * 9 + (dy + 1) * 3 + (dz + 1), two a 16-bit component; byte 27
// is flags). WHICH cells a vertex blends is a property of the model, not of
// the instance, so it rides in the shared mesh: the vertex alpha byte is the
// vertex's light recipe (BlockCubeEntityRenderer::BuildFallingMesh):
//
//   1ccc dddd   smooth: d = the quad's Direction ordinal (MC order
//               Down Up North South West East), c bit 3 = faceCubic
//               (bits 4-6 zero)
//   0000 0nnn   flat: n = the cell — a Direction ordinal (own cell +
//               that direction), 6 = the own cell
//   1111 1111   flat, own cell (every other mesh: TNT, a moving block,
//               the cube fallback — the builder's opaque alpha)
//
// AO is the render state's (all-AIR neighbours: a pure function of the
// model) and is baked into the vertex colour with the tint and face shade.
// The lightmap is MC's (Render::Lightmap), sampled per vertex exactly as the
// terrain shaders sample it.

layout (location = 0) in vec3 aPos;       // Model-space position
layout (location = 1) in vec2 aTexCoord;  // Texture coordinates from atlas
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized by GL); a = light recipe

// Per-instance: xyz = world translation, w = uniform scale.
layout (location = 3) in vec4 aInstance;
// Per-instance: the 27 cells' packed light, two bytes a 16-bit component
// (unsigned shorts, not normalized: whole numbers 0..65535 in a float).
layout (location = 4) in vec4 aCells0;    // cells  0..7
layout (location = 5) in vec4 aCells1;    // cells  8..15
layout (location = 6) in vec4 aCells2;    // cells 16..23
layout (location = 7) in vec2 aCells3;    // cells 24..26, flags

uniform mat4 uViewProj;         // View-projection only; model is per-instance
uniform vec4 uPortalClipPlane;  // See block.vert for the contract

// MC's lightmap (Render::Lightmap), texture unit 3: 16x16, x = block light,
// y = sky light, LINEAR.
uniform sampler2D uLightmap;

out vec2 fragTexCoord;
out vec3 fragWorldPos;
out vec4 fragColor;

uint g_cells[14];

// MC sample_lightmap.glsl: uv are the light coords (0..240 each).
vec3 sampleLightmap(vec2 uv) {
    return texture(uLightmap, clamp(uv / 256.0 + 0.5 / 16.0, vec2(0.5 / 16.0), vec2(15.5 / 16.0))).rgb;
}

// (block, sky) levels of the cell at `off` from blockPos.
ivec2 cellLight(ivec3 off) {
    int i = (off.x + 1) * 9 + (off.y + 1) * 3 + (off.z + 1);
    uint v = g_cells[i >> 1];
    uint b = ((i & 1) != 0) ? (v >> 8u) : (v & 255u);
    return ivec2(int(b & 15u), int(b >> 4u));
}

// Direction ordinal -> step (MC Direction.getStepX/Y/Z).
ivec3 dirStep(int d) {
    if (d == 0) return ivec3( 0, -1,  0);   // Down
    if (d == 1) return ivec3( 0,  1,  0);   // Up
    if (d == 2) return ivec3( 0,  0, -1);   // North
    if (d == 3) return ivec3( 0,  0,  1);   // South
    if (d == 4) return ivec3(-1,  0,  0);   // West
    return ivec3(1, 0, 0);                  // East
}

// MC BlockModelLighter.AdjacencyInfo.corners (BlockModelLighter.hpp kCorners).
int cornerDir(int d, int i) {
    int kCorners[24] = int[24](
        4, 5, 2, 3,     // Down:  West East North South
        5, 4, 2, 3,     // Up:    East West North South
        1, 0, 5, 4,     // North: Up Down East West
        4, 5, 0, 1,     // South: West East Down Up
        1, 0, 2, 3,     // West:  Up Down North South
        0, 1, 2, 3);    // East:  Down Up North South
    return kCorners[d * 4 + i];
}

// BlockModelLighter::CornerWeight: the bilinear weight of a corner on the
// `d` side at block-local vertex position p.
float cornerWeight(int d, vec3 p) {
    int axis = (d >= 4) ? 0 : ((d <= 1) ? 1 : 2);
    bool positive = (d == 1 || d == 3 || d == 5);
    float c = clamp(p[axis], 0.0, 1.0);
    return positive ? c : 1.0 - c;
}

// MC LightCoordsUtil.smoothBlend (LightCoords::SmoothBlend) on (block, sky)
// levels; the result is in smooth light coords (0..240 each).
ivec2 smoothBlend(ivec2 n1, ivec2 n2, ivec2 n3, ivec2 c) {
    if (c.y > 2 || c.x > 2) {
        if (n1 == ivec2(0)) n1 = c; else if (n1.y == 0) n1.y = c.y;
        if (n2 == ivec2(0)) n2 = c; else if (n2.y == 0) n2.y = c.y;
        if (n3 == ivec2(0)) n3 = c; else if (n3.y == 0) n3.y = c.y;
    }
    return (n1 + n2 + n3 + c) * 4;
}

// MC BlockModelLighter.prepareQuadAmbientOcclusion's light half over the
// MovingBlockRenderState: every neighbour is AIR, so every side lets light
// through (all four diagonals are read) and the centre is always the cell
// the quad faces. BlockModelLighter::QuadLightSmooth with that level.
vec2 smoothLight(int d, bool cubic, vec3 p) {
    ivec3 face = dirStep(d);
    ivec3 base = cubic ? face : ivec3(0);
    ivec3 c0 = dirStep(cornerDir(d, 0));
    ivec3 c1 = dirStep(cornerDir(d, 1));
    ivec3 c2 = dirStep(cornerDir(d, 2));
    ivec3 c3 = dirStep(cornerDir(d, 3));
    ivec2 l0 = cellLight(base + c0);
    ivec2 l1 = cellLight(base + c1);
    ivec2 l2 = cellLight(base + c2);
    ivec2 l3 = cellLight(base + c3);
    ivec2 center = cellLight(face);
    ivec2 tc0 = smoothBlend(l3, l0, cellLight(base + c0 + c3), center);
    ivec2 tc1 = smoothBlend(l2, l0, cellLight(base + c0 + c2), center);
    ivec2 tc2 = smoothBlend(l2, l1, cellLight(base + c1 + c2), center);
    ivec2 tc3 = smoothBlend(l3, l1, cellLight(base + c1 + c3), center);
    // kCornerSides: (c3, c0), (c2, c0), (c2, c1), (c3, c1).
    float w0 = cornerWeight(cornerDir(d, 3), p) * cornerWeight(cornerDir(d, 0), p);
    float w1 = cornerWeight(cornerDir(d, 2), p) * cornerWeight(cornerDir(d, 0), p);
    float w2 = cornerWeight(cornerDir(d, 2), p) * cornerWeight(cornerDir(d, 1), p);
    float w3 = cornerWeight(cornerDir(d, 3), p) * cornerWeight(cornerDir(d, 1), p);
    // MC LightCoordsUtil.smoothWeightedBlend: truncated per channel.
    int sky   = int(float(tc0.y) * w0 + float(tc1.y) * w1 + float(tc2.y) * w2 + float(tc3.y) * w3);
    int block = int(float(tc0.x) * w0 + float(tc1.x) * w1 + float(tc2.x) * w2 + float(tc3.x) * w3);
    return vec2(float(block & 255), float(sky & 255));
}

void main() {
    vec4 worldPos = vec4(aInstance.xyz + aInstance.w * aPos, 1.0);

    gl_Position = uViewProj * worldPos;
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, worldPos.xyz) + uPortalClipPlane.w
        : 1.0;
    fragTexCoord = aTexCoord;
    fragWorldPos = worldPos.xyz;

    g_cells[0]  = uint(aCells0.x); g_cells[1]  = uint(aCells0.y);
    g_cells[2]  = uint(aCells0.z); g_cells[3]  = uint(aCells0.w);
    g_cells[4]  = uint(aCells1.x); g_cells[5]  = uint(aCells1.y);
    g_cells[6]  = uint(aCells1.z); g_cells[7]  = uint(aCells1.w);
    g_cells[8]  = uint(aCells2.x); g_cells[9]  = uint(aCells2.y);
    g_cells[10] = uint(aCells2.z); g_cells[11] = uint(aCells2.w);
    g_cells[12] = uint(aCells3.x); g_cells[13] = uint(aCells3.y);

    // Flags (byte 27), bit 0: the vertex colours already carry the light (a
    // moving block lit like the section mesh) — multiply by nothing.
    bool preLit = ((g_cells[13] >> 8u) & 1u) != 0u;
    vec3 light = vec3(1.0);
    if (!preLit) {
        int recipe = int(aColor.a * 255.0 + 0.5);
        vec2 coords;
        if (recipe != 255 && recipe >= 128) {
            coords = smoothLight(recipe & 7, (recipe & 8) != 0, aPos);
        } else {
            int n = (recipe == 255) ? 6 : (recipe & 7);
            ivec2 l = cellLight(n == 6 ? ivec3(0) : dirStep(n));
            coords = vec2(l * 16);
        }
        light = sampleLightmap(coords);
    }
    fragColor = vec4(aColor.rgb * light, 1.0);
}
