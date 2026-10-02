// File: shaders/block_instanced_vk.vert (Vulkan twin of block_instanced.vert)
//
// Same contract as the GL shader: per-instance transform as ONE vec4 at
// attribute location 3 (xyz world translation, w uniform scale — see
// block_instanced.vert for why that is the whole transform space),
// view-projection in the push-constant uMVP slot (the backend routes the name
// "uViewProj" there), fragment shader = block_vk.frag under the PORTAL
// pipeline layout so the fog/environment CommonUBO is bound.
//
// Light per vertex from the instance's 3x3x3 cell light and the vertex's
// light recipe (its alpha byte) — see block_instanced.vert for the whole
// contract; the function bodies are the GL shader's verbatim. The cells
// arrive as R16G16B16A16_UINT / R16G16_UINT (Vulkan needs the shader-side
// attribute to be unsigned integer for a _UINT format), the lightmap on
// descriptor set 5 (texture slot 3), as the terrain shaders read it.
//
// NOT in CMake's VK_SHADERS list: the committed .spv beside this file is what
// ships. Recompile it by hand after any edit:
//     glslc shaders/block_instanced_vk.vert -o shaders/block_instanced_vk.vert.spv
#version 450

layout (location = 0) in vec3 aPos;       // MODEL-space position
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec4 aColor;     // RGBA8 normalized; a = light recipe

// Per-instance: xyz = world translation, w = uniform scale.
layout (location = 3) in vec4 aInstance;
// Per-instance: the 27 cells' packed light, two bytes a 16-bit component.
layout (location = 4) in uvec4 aCells0;   // cells  0..7
layout (location = 5) in uvec4 aCells1;   // cells  8..15
layout (location = 6) in uvec4 aCells2;   // cells 16..23
layout (location = 7) in uvec2 aCells3;   // cells 24..26, flags

layout (push_constant) uniform PushConstants {
    mat4 uMVP;              // HOLDS uViewProj for this shader (model is per-instance)
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uPortalClipPlane;  // xyz = plane normal, w = -dot(normal, point)
} pc;

// MC's lightmap (Render::Lightmap), texture slot 3 = descriptor set 5:
// 16x16, x = block light, y = sky light, LINEAR.
layout (set = 5, binding = 0) uniform sampler2D uLightmap;

layout (location = 0) out vec2 fragTexCoord;
layout (location = 1) out vec3 fragWorldPos;
layout (location = 2) out vec4 fragColor;

out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

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

    gl_Position  = pc.uMVP * worldPos;
    fragTexCoord = aTexCoord;
    // World-space, from the TRANSFORMED position — aPos is model-space here,
    // unlike block_vk.vert where the mesher already emits world-space.
    fragWorldPos = worldPos.xyz;

    g_cells[0]  = aCells0.x; g_cells[1]  = aCells0.y;
    g_cells[2]  = aCells0.z; g_cells[3]  = aCells0.w;
    g_cells[4]  = aCells1.x; g_cells[5]  = aCells1.y;
    g_cells[6]  = aCells1.z; g_cells[7]  = aCells1.w;
    g_cells[8]  = aCells2.x; g_cells[9]  = aCells2.y;
    g_cells[10] = aCells2.z; g_cells[11] = aCells2.w;
    g_cells[12] = aCells3.x; g_cells[13] = aCells3.y;

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

    gl_ClipDistance[0] = dot(worldPos.xyz, pc.uPortalClipPlane.xyz) + pc.uPortalClipPlane.w;
}
