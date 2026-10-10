// File: shaders/terrain_rec_vk.vert (Vulkan twin of terrain_rec.vert)
// The QUAD-RECORD terrain vertex shader (docs/quad-records.md, Vertex.hpp
// QuadRecord): a cube face is one 16-byte record in the slab's face map,
// not four vertices. There is NO vertex input (VertexLayout::noVertexInput):
// the draw runs over the shared index pattern 0 1 2 0 2 3 per quad with
// baseVertex = 4 x the run's first record texel, so gl_VertexIndex >> 2 is
// the record's texel and gl_VertexIndex & 3 the corner. The corner's
// position and tile uv are rebuilt here exactly as Mesher::FlushGreedyQuads
// (emitMerged) lays them out, and the outputs are the ones terrain_vk.vert
// emits, so the three terrain fragment shaders are shared.
//
// SectionOrigins is descriptor set 3, the face map set 4 (the texel buffer
// the fragment shaders read block records from), the lightmap set 5.
#version 450

#ifdef TERRAIN_F16
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require
#define hvec2 f16vec2
#define hvec3 f16vec3
#define hvec4 f16vec4
#else
#define hvec2 vec2
#define hvec3 vec3
#define hvec4 vec4
#endif

// Push constants — must match C++ PushConstantBlock (see terrain_vk.vert).
layout (push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uPortalClipPlane;
} pc;

// INTEGER block positions — see terrain.vert.
layout (std140, set = 3, binding = 0) uniform SectionOrigins {
    ivec4 uOrigins[1024];
};

// The slab's records: one RGBA32UI texel per quad record or face-map
// block record (ChunkMegaBuffer's buffer texture over the slab VBO).
layout (set = 4, binding = 0) uniform usamplerBuffer uFaceMap;

// MC's lightmap (Render::Lightmap), texture slot 3 = descriptor set 5.
layout (set = 5, binding = 0) uniform sampler2D uLightmap;

vec3 sampleLightmap(vec2 uv) {
    return texture(uLightmap, clamp(uv / 256.0 + 0.5 / 16.0, vec2(0.5 / 16.0), vec2(15.5 / 16.0))).rgb;
}

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
    vec4  uOverlayColor_;
    ivec4 uRenderOrigin_;
} U;

// Output to fragment shader — identical to terrain_vk.vert's.
layout (location = 0) out vec2 fragTexCoord;
layout (location = 1) out hvec2 fragFog;
layout (location = 2) out hvec4 fragColor;
layout (location = 3) flat out int fragSprite;
layout (location = 4) flat out int fragRecord;
layout (location = 5) flat out int fragAux;

out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

void main() {
    const int vid = gl_VertexIndex;
    const uvec4 r = texelFetch(uFaceMap, vid >> 2);
    const int corner = vid & 3;

    // QuadRecord (Vertex.hpp): block, side, mode, size, slot, sprite.
    const int bx = int(r.x & 15u), by = int((r.x >> 4) & 15u), bz = int((r.x >> 8) & 15u);
    const int side = int((r.x >> 12) & 7u);          // 0..5 = -X +X -Y +Y -Z +Z
    const bool rect = (r.x & 0x8000u) != 0u;
    const int w = rect ? int((r.x >> 16) & 15u) + 1 : 1;
    const int h = rect ? int((r.x >> 20) & 15u) + 1 : 1;
    const int slot = int(r.y & 0x3FFu);
    const int axis = side >> 1;                     // 0 x, 1 y, 2 z
    const bool positive = (side & 1) != 0;

    // The greedy grid (Greedy::CellOf): the face's plane and its (u, v) cell.
    int u0, v0, plane;
    if (axis == 1)      { plane = by; u0 = bx; v0 = bz; }
    else if (axis == 2) { plane = bz; u0 = bx; v0 = by; }
    else                { plane = bx; u0 = bz; v0 = by; }
    const int u1 = u0 + w, v1 = v0 + h;

    // This corner in grid terms — emitMerged's winding per face:
    //   +Y:          (u0,v1) (u1,v1) (u1,v0) (u0,v0)
    //   -Y, +Z, -X:  (u0,v0) (u1,v0) (u1,v1) (u0,v1)
    //   -Z, +X:      (u1,v0) (u0,v0) (u0,v1) (u1,v1)
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
    const int p = positive ? plane + 1 : plane;
    vec3 local;
    if (axis == 1)      local = vec3(float(gu), float(p),  float(gv));
    else if (axis == 2) local = vec3(float(gu), float(gv), float(p));
    else                local = vec3(float(p),  float(gv), float(gu));

    // Tile-space uv from the local position, per face (emitMerged's tileUV),
    // and the rectangle's tile-space origin (its cellTile minimum).
    vec2 tile;
    int tu0, tv0;
    switch (side) {
        case 3:  tile = vec2(local.x,        local.z);        tu0 = u0;      tv0 = v0;      break;   // +Y
        case 2:  tile = vec2(local.x,        16.0 - local.z); tu0 = u0;      tv0 = 16 - v1; break;   // -Y
        case 5:  tile = vec2(local.x,        16.0 - local.y); tu0 = u0;      tv0 = 16 - v1; break;   // +Z
        case 4:  tile = vec2(16.0 - local.x, 16.0 - local.y); tu0 = 16 - u1; tv0 = 16 - v1; break;   // -Z
        case 1:  tile = vec2(16.0 - local.z, 16.0 - local.y); tu0 = 16 - u1; tv0 = 16 - v1; break;   // +X
        default: tile = vec2(local.z,        16.0 - local.y); tu0 = u0;      tv0 = 16 - v1; break;   // -X
    }

    // Render-space position (world minus the view's origin, exact).
    const vec3 worldPos = vec3(uOrigins[slot].xyz - U.uRenderOrigin_.xyz) + local;
    const int fadeStart = uOrigins[slot].w;
    const int fadeMs    = int(U.uScalarsC_.z);
    const float visibility = (fadeMs <= 0 || fadeStart == 0)
        ? 1.0
        : clamp(float(U.uRenderOrigin_.w - fadeStart) / float(fadeMs), 0.0, 1.0);

    gl_Position = pc.uMVP * vec4(worldPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(pc.uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(pc.uPortalClipPlane.xyz, worldPos) + pc.uPortalClipPlane.w
        : 1.0;

    fragTexCoord = tile;
    fragAux = 0;
    vec4 color = vec4(1.0);
    if (rect) {
        // A face-mapped rectangle: the fragment shader shades every block
        // from its own face-map record (terrain_opaque_vk.frag).
        fragSprite = 0x40000 | w | (h << 5) | (tv0 << 10) | (tu0 << 14);
        fragRecord = int(r.w);
    } else {
        // A single face: tint x face shade, this corner's AO level and its
        // light — what an unmerged quad's vertex carries (MC terrain.vsh:
        // vertexColor = Color * sample_lightmap(UV2)).
        fragSprite = int((r.y >> 10) & 0xFFFFu);
        fragRecord = 0;
        const int tu = int(tile.x) - tu0, tv = int(tile.y) - tv0;   // the tile corner, 0/1 each
        const int c = tu + 2 * tv;
        const float ao = 1.0 - 0.2 * float((r.z >> uint(24 + 2 * c)) & 3u);
        uint l12;
        if (c == 0)      l12 = r.w & 0xFFFu;
        else if (c == 1) l12 = (r.w >> 12) & 0xFFFu;
        else if (c == 2) l12 = ((r.w >> 24) & 0xFFu) | (((r.x >> 16) & 0xFu) << 8);
        else             l12 = (r.x >> 20) & 0xFFFu;
        color.rgb = vec3(float(r.z & 0xFFu), float((r.z >> 8) & 0xFFu), float((r.z >> 16) & 0xFFu)) * (ao / 255.0);
        if (U.uScalarsD_.w > 0.5) {
            color.rgb *= sampleLightmap(vec2(float((l12 & 63u) * 4u), float((l12 >> 6) * 4u)));
        }
    }

    if (U.uFogColor_.a > 0.0) {
        const vec3 camDelta = worldPos - U.uCamPosBright_.xyz;
        fragFog = hvec2(vec2(length(camDelta), max(length(camDelta.xz), abs(camDelta.y))));
    } else {
        fragFog = hvec2(0.0);
    }
    fragColor = hvec4(color);
    fragAux |= int(visibility * 255.0 + 0.5) << 16;
}
