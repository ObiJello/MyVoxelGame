// File: shaders/terrain_vk.vert (Vulkan twin of terrain.vert)
// Chunk-TERRAIN vertex shader for the packed 20-byte TerrainVertex — see
// terrain.vert for the attribute encoding. ChunkRenderer registers
// GetTerrainVertexLayout() on the three terrain shaders so their pipelines
// bake the 16-byte input; the shared block_vk.vert must NOT be pointed at
// terrain buffers (entity renderers feed it 24-byte vertices).
//
// SectionOrigins is descriptor set 3 of the portal pipeline layout — the
// backend's user uniform block (RenderBackend::BindUniformBuffer), bound by
// ChunkMegaBuffer::BindSlab with the slab's 16 KB window as dynamic offset.
#version 450

// Half-precision varyings (TERRAIN_F16 — terrain_f16_vk.vert.spv, paired with
// the _f16 fragment variants on Metal and on a Vulkan device with
// shaderFloat16 + storageInputOutput16): the interpolated colour and the
// two fog distances go out as float16, 24 bytes a vertex fewer in the
// post-transform stream the TBDR writes and the fragment stage reads
// back (the "shaded vertex read" limiter, 2026-10-08). Texture coordinates
// stay float: tile space runs to 16 and half's step there is a quarter
// texel. Fog in half: a 0.25-block step at 512 blocks, 0.0025 of the fog
// band.
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

layout (location = 0) in vec4 aPosSlot;   // px py pz slot (R16G16B16A16_UNORM)
layout (location = 1) in vec2 aTexCoord;  // u v (R16G16_UNORM)
layout (location = 2) in vec4 aColor;     // RGBA8 normalized
layout (location = 3) in vec4 aLight;     // block8, sky8, reserved (R8G8B8A8_UNORM)

// Push constants — must match C++ PushConstantBlock layout exactly (see
// block_vk.vert for the uPortalClipPlane aliasing note).
layout (push_constant) uniform PushConstants {
    mat4 uMVP;              // 64 bytes
    vec2 uScreenSize;       // 8 bytes
    float uLineWidth;       // 4 bytes
    float uAlphaTest;       // 4 bytes
    vec4 uPortalClipPlane;  // 16 bytes — xyz = world-space plane normal,
                            //            w = -dot(normal, pointOnPlane)
} pc;

// INTEGER block positions — see terrain.vert.
layout (std140, set = 3, binding = 0) uniform SectionOrigins {
    ivec4 uOrigins[1024];
};

// MC's lightmap (Render::Lightmap), texture slot 3 = descriptor set 5:
// 16x16, x = block light, y = sky light, LINEAR.
layout (set = 5, binding = 0) uniform sampler2D uLightmap;

// MC sample_lightmap.glsl: uv are the light coords (0..240 each).
vec3 sampleLightmap(vec2 uv) {
    return texture(uLightmap, clamp(uv / 256.0 + 0.5 / 16.0, vec2(0.5 / 16.0), vec2(15.5 / 16.0))).rgb;
}

// Common UBO (set = 1), the same layout the terrain fragment shaders
// declare, plus the render origin appended at 368 (VKBackend::CommonUBO).
// Only uRenderOrigin_ is read here: camera-relative rendering, see
// terrain.vert and RenderOrigin.hpp.
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

// Output to fragment shader
layout (location = 0) out vec2 fragTexCoord;
layout (location = 1) out hvec2 fragFog;        // MC terrain.vsh: (spherical, cylindrical) fog distance, interpolated
layout (location = 2) out hvec4 fragColor;
layout (location = 3) flat out int fragSprite;   // see the decode below; -1 = untiled
layout (location = 4) flat out int fragRecord;   // face-mapped: texel index of the first record
layout (location = 5) flat out int fragAux;      // two-sided: back mapping (alpha byte); bits 16..23 = MC ChunkVisibility (the section's fade-in, 0..255)

// Explicit gl_PerVertex redeclaration so gl_ClipDistance[0] actually lands —
// see the long note in block_vk.vert.
out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

void main() {
    int slotRaw = int(aPosSlot.w * 65535.0 + 0.5);
    int slot = slotRaw & 0x3FF;                 // origin-table row (1024 per slab)
    bool tiled = (slotRaw & 0x8000) != 0;       // greedy-merged: tile-space uv
    bool mapped = (slotRaw & 0x4000) != 0;      // ... with per-block records in the face map
    bool twoSided = (slotRaw & 0x2000) != 0;    // tiled, drawn without culling, u mirrored on the back

    vec3 rel = aPosSlot.xyz * (65535.0 / 2048.0) - 2.0;
    // Render-space position (world minus the view's origin, exact).
    vec3 worldPos = vec3(uOrigins[slot].xyz - U.uRenderOrigin_.xyz) + rel;
    // MC chunk fade-in (ChunkVisibility): the row's .w is the section's
    // first-upload time on the SectionFade clock; U.uRenderOrigin_.w is
    // the clock now and U.uScalarsC_.z the fade length, in milliseconds.
    int fadeStart = uOrigins[slot].w;
    int fadeMs    = int(U.uScalarsC_.z);
    float visibility = (fadeMs <= 0 || fadeStart == 0)
        ? 1.0
        : clamp(float(U.uRenderOrigin_.w - fadeStart) / float(fadeMs), 0.0, 1.0);

    gl_Position = pc.uMVP * vec4(worldPos, 1.0);
    // Portal-plane clipping — same contract and rationale as block_vk.vert.
    gl_ClipDistance[0] = (any(notEqual(pc.uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(pc.uPortalClipPlane.xyz, worldPos) + pc.uPortalClipPlane.w
        : 1.0;

    // fragSprite, per mode (all flat, so the fragment shader branches per
    // primitive):
    //   untiled       -1
    //   tiled (fluid) sprite id (bits 0..15); bit 17 = two-sided, bits
    //                 19..24 = its front-normal code, bit 25 = emissive;
    //                 fragAux = the back mapping (TerrainVertex::TwoSided's
    //                 alpha byte)
    //   face-mapped   bit 18 set; bits 0..4 = w, 5..9 = h, 10..13 = tv0,
    //                 14..17 = tu0 — the rectangle's tile-space size and
    //                 origin; fragRecord = texel index of its first record.
    fragRecord = 0;
    fragAux = 0;
    if (mapped) {
        int packedU = int(aTexCoord.x * 65535.0 + 0.5);
        int packedV = int(aTexCoord.y * 65535.0 + 0.5);
        fragTexCoord = vec2(float(packedU & 0x1F), float((packedU >> 5) & 0x1F));
        int tu0 = (packedU >> 10) & 0xF;
        fragSprite = 0x40000 | (packedV & 0x3FF) | (((packedV >> 10) & 0xF) << 10) | (tu0 << 14);
        // The record index arrives as the colour attribute's four bytes.
        ivec4 c = ivec4(aColor * 255.0 + 0.5);
        fragRecord = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
    } else if (tiled) {
        int packedTile = int(aTexCoord.x * 65535.0 + 0.5);
        if (twoSided) {
            // u = tileU16 | tileV16 << 5 | normalCode << 10, alpha byte = the
            // face's u-extent sum in sixteenths (TerrainVertex::TwoSided).
            fragTexCoord = vec2(float(packedTile & 0x1F), float((packedTile >> 5) & 0x1F)) / 16.0;
            // v bit 15 = emissive (Vertex.hpp, EMISSIVE QUADS: the alpha
            // byte is taken by the back mapping); it moves to bit 25 so the
            // sprite id in bits 0..15 stays clean.
            int spriteV = int(aTexCoord.y * 65535.0 + 0.5);
            fragSprite = (spriteV & 0x7FFF) | 0x20000 | (((packedTile >> 10) & 0x3F) << 19)
                       | ((spriteV & 0x8000) << 10);
            fragAux = int(aColor.a * 255.0 + 0.5);
            // Bit 8: the camera is behind the quad. dot(n, cam - p) has
            // one sign over a plane, so the fragment shader's per-pixel
            // test moves here, flat (terrain_opaque_vk.frag mirrors u/v
            // on the back from it).
            int code = (packedTile >> 10) & 0x3F;
            vec3 n = vec3(float((code & 3) == 1) - float((code & 3) == 3),
                          float(((code >> 2) & 3) == 1) - float(((code >> 2) & 3) == 3),
                          float(((code >> 4) & 3) == 1) - float(((code >> 4) & 3) == 3));
            if (dot(n, U.uCamPosBright_.xyz - worldPos) < 0.0) fragAux |= 0x100;
        } else {
            fragTexCoord = vec2(float(packedTile & 0xFF), float(packedTile >> 8));
            fragSprite = int(aTexCoord.y * 65535.0 + 0.5);
        }
    } else {
        fragTexCoord = aTexCoord;
        fragSprite = -1;
    }
    // MC terrain.vsh: fog_spherical_distance / fog_cylindrical_distance are
    // PER VERTEX and interpolated (fog.glsl); the fragment shader only
    // applies them. Two varying floats instead of the position's three,
    // and no length() per fragment.
    // Fog off (uFogColor.a = 0): no distances to measure, the fragment
    // shader skips the fog on the same flag.
    if (U.uFogColor_.a > 0.0) {
        vec3 camDelta = worldPos - U.uCamPosBright_.xyz;
        fragFog = hvec2(vec2(length(camDelta), max(length(camDelta.xz), abs(camDelta.y))));
    } else {
        fragFog = hvec2(0.0);
    }
    vec4 color = aColor;
    // MC terrain.vsh: vertexColor = Color * sample_lightmap(Sampler2, UV2);
    // a face-mapped rectangle lights per block in the fragment shader (see
    // terrain.vert). World Lighting off (U.uScalarsD_.w = 0): the lightmap
    // is white, so the sample is skipped.
    if (!mapped && U.uScalarsD_.w > 0.5) color.rgb *= sampleLightmap(aLight.rg * 255.0);
    fragColor = hvec4(color);
    // The fade rides in fragAux's bits 16..23 (a flat varying fewer).
    fragAux |= int(visibility * 255.0 + 0.5) << 16;
}
