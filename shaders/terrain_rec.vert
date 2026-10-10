// File: shaders/terrain_rec.vert (OpenGL twin of terrain_rec_vk.vert)
// The QUAD-RECORD terrain vertex shader: a cube face is one 16-byte record
// in the slab's face map (Vertex.hpp QuadRecord, docs/quad-records.md). No
// vertex attributes: the draw runs over the shared index pattern with
// baseVertex = 4 x the run's first record texel, so gl_VertexID >> 2 is the
// record and & 3 the corner. Keep in step with terrain_rec_vk.vert and the
// uniforms of terrain.vert.
#version 330 core

uniform mat4 uMVP;
uniform vec4 uPortalClipPlane;
layout (std140) uniform SectionOrigins {
    ivec4 uOrigins[1024];
};
uniform ivec3 uRenderOrigin;
uniform vec3 uCameraPos;
uniform int uFadeNowMs;
uniform int uFadeMs;
uniform sampler2D uLightmap;
uniform int uWorldLighting;
uniform vec4 uFogColor;
// The slab's records, texture unit 2 (ChunkRenderer::BindSpriteTable sets
// the sampler): one RGBA32UI texel per record.
uniform usamplerBuffer uFaceMap;

vec3 sampleLightmap(vec2 uv) {
    return texture(uLightmap, clamp(uv / 256.0 + 0.5 / 16.0, vec2(0.5 / 16.0), vec2(15.5 / 16.0))).rgb;
}

out vec2 fragTexCoord;
out vec2 fragFog;
out vec4 fragColor;
flat out int fragSprite;
flat out int fragRecord;
flat out int fragAux;

void main() {
    int vid = gl_VertexID;
    uvec4 r = texelFetch(uFaceMap, vid >> 2);
    int corner = vid & 3;

    int bx = int(r.x & 15u), by = int((r.x >> 4) & 15u), bz = int((r.x >> 8) & 15u);
    int side = int((r.x >> 12) & 7u);          // 0..5 = -X +X -Y +Y -Z +Z
    bool rect = (r.x & 0x8000u) != 0u;
    int w = rect ? int((r.x >> 16) & 15u) + 1 : 1;
    int h = rect ? int((r.x >> 20) & 15u) + 1 : 1;
    int slot = int(r.y & 0x3FFu);
    int axis = side >> 1;
    bool positive = (side & 1) != 0;

    int u0, v0, plane;
    if (axis == 1)      { plane = by; u0 = bx; v0 = bz; }
    else if (axis == 2) { plane = bz; u0 = bx; v0 = by; }
    else                { plane = bx; u0 = bz; v0 = by; }
    int u1 = u0 + w, v1 = v0 + h;

    int gu, gv;
    if (side == 3) {
        gu = (corner == 1 || corner == 2) ? u1 : u0;
        gv = (corner <= 1) ? v1 : v0;
    } else if (side == 4 || side == 1) {
        gu = (corner == 0 || corner == 3) ? u1 : u0;
        gv = (corner >= 2) ? v1 : v0;
    } else {
        gu = (corner == 1 || corner == 2) ? u1 : u0;
        gv = (corner >= 2) ? v1 : v0;
    }
    int p = positive ? plane + 1 : plane;
    vec3 local;
    if (axis == 1)      local = vec3(float(gu), float(p),  float(gv));
    else if (axis == 2) local = vec3(float(gu), float(gv), float(p));
    else                local = vec3(float(p),  float(gv), float(gu));

    vec2 tile;
    int tu0, tv0;
    if (side == 3)      { tile = vec2(local.x,        local.z);        tu0 = u0;      tv0 = v0;      }
    else if (side == 2) { tile = vec2(local.x,        16.0 - local.z); tu0 = u0;      tv0 = 16 - v1; }
    else if (side == 5) { tile = vec2(local.x,        16.0 - local.y); tu0 = u0;      tv0 = 16 - v1; }
    else if (side == 4) { tile = vec2(16.0 - local.x, 16.0 - local.y); tu0 = 16 - u1; tv0 = 16 - v1; }
    else if (side == 1) { tile = vec2(16.0 - local.z, 16.0 - local.y); tu0 = 16 - u1; tv0 = 16 - v1; }
    else                { tile = vec2(local.z,        16.0 - local.y); tu0 = u0;      tv0 = 16 - v1; }

    vec3 worldPos = vec3(uOrigins[slot].xyz - uRenderOrigin) + local;
    int fadeStart = uOrigins[slot].w;
    float visibility = (uFadeMs <= 0 || fadeStart == 0)
        ? 1.0
        : clamp(float(uFadeNowMs - fadeStart) / float(uFadeMs), 0.0, 1.0);

    gl_Position = uMVP * vec4(worldPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, worldPos) + uPortalClipPlane.w
        : 1.0;

    fragTexCoord = tile;
    fragAux = 0;
    vec4 color = vec4(1.0);
    if (rect) {
        fragSprite = 0x40000 | w | (h << 5) | (tv0 << 10) | (tu0 << 14);
        fragRecord = int(r.w);
    } else {
        fragSprite = int((r.y >> 10) & 0xFFFFu);
        fragRecord = 0;
        int tu = int(tile.x) - tu0, tv = int(tile.y) - tv0;
        int c = tu + 2 * tv;
        float ao = 1.0 - 0.2 * float((r.z >> uint(24 + 2 * c)) & 3u);
        uint l12;
        if (c == 0)      l12 = r.w & 0xFFFu;
        else if (c == 1) l12 = (r.w >> 12) & 0xFFFu;
        else if (c == 2) l12 = ((r.w >> 24) & 0xFFu) | (((r.x >> 16) & 0xFu) << 8);
        else             l12 = (r.x >> 20) & 0xFFFu;
        color.rgb = vec3(float(r.z & 0xFFu), float((r.z >> 8) & 0xFFu), float((r.z >> 16) & 0xFFu)) * (ao / 255.0);
        if (uWorldLighting != 0) {
            color.rgb *= sampleLightmap(vec2(float((l12 & 63u) * 4u), float((l12 >> 6) * 4u)));
        }
    }

    if (uFogColor.a > 0.0) {
        vec3 camDelta = worldPos - uCameraPos;
        fragFog = vec2(length(camDelta), max(length(camDelta.xz), abs(camDelta.y)));
    } else {
        fragFog = vec2(0.0);
    }
    fragColor = color;
    fragAux |= int(visibility * 255.0 + 0.5) << 16;
}
