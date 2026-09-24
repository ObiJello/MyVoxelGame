// File: shaders/terrain.vert
// Chunk-TERRAIN vertex shader for the packed 20-byte TerrainVertex (see
// src/client/renderer/core/Vertex.hpp and GetTerrainVertexLayout()).
//
// Every attribute arrives normalized; the integers are recovered with
// value * 65535 (exact in fp32), so no integer attribute path is needed.
//   aPosSlot.xyz  section-relative position, fixed point 1/2048 with a
//                 2-block bias; the section's world origin is looked up in
//                 SectionOrigins by aPosSlot.w's low 15 bits (the slab-local
//                 row the mega buffer patched in at upload). Bit 15 = TILED.
//   aTexCoord     untiled: atlas uv (unorm16).
//                 tiled:   x = tileU | tileV << 8 (integer tile-space corner,
//                          0..16 repeats), y = sprite id (see uSpriteTable
//                          in the fragment shaders).
//                 face-mapped (bit 14): x = tileU | tileV << 5 | tu0 << 10,
//                          y = w | h << 5 | tv0 << 10, and aColor is the
//                          record texel index (see Vertex.hpp).
//   aLight        r = block light, g = sky light, MC light coords 0..240
//                 (level * 16) — sampled from the lightmap exactly as MC's
//                 sample_lightmap, per vertex (terrain.vsh).
//
// Terrain-ONLY. Do not point entity/item/portal renderers at this shader:
// they feed 24-byte block.vert buffers. Keep the rest in step with block.vert.
#version 330 core

layout (location = 0) in vec4 aPosSlot;   // px py pz slot (4 x unorm16)
layout (location = 1) in vec2 aTexCoord;  // u v (2 x unorm16)
layout (location = 2) in vec4 aColor;     // RGBA8 normalized
layout (location = 3) in vec4 aLight;     // block8, sky8, reserved (RGBA8 normalized)

// Uniforms
uniform mat4 uMVP;  // Model-View-Projection matrix
// World-space clip plane for portal see-through rendering — same contract
// as block.vert (see the comment there).
uniform vec4 uPortalClipPlane;
// Per-slab section-origin table (ChunkMegaBuffer::BindSlab), block binding
// point 0 — 1024 rows = the 16 KB every implementation guarantees. INTEGER
// block positions: exact anywhere in the world, where a float is not past
// ±16.7 M blocks.
layout (std140) uniform SectionOrigins {
    ivec4 uOrigins[1024];
};
// Camera-relative rendering (RenderOrigin.hpp): the view's origin, an
// integer block position. The section origin minus it is computed in
// INTEGER arithmetic — exact — and only the small result meets the float
// vertex offset, so terrain at x = 300,000 is as precise as terrain at the
// origin. uMVP is the render-space view (its translation is the camera's
// sub-block offset), uPortalClipPlane is in render space too.
uniform ivec3 uRenderOrigin;
// MC chunk fade-in (Options.chunkSectionFadeInTime, ChunkVisibility): the
// origin row's .w is the section's first-upload time on the SectionFade
// clock, these are the clock now and the fade length, all in milliseconds.
uniform int uFadeNowMs;
uniform int uFadeMs;
// MC's lightmap (Render::Lightmap), texture unit 3: 16x16, x = block light,
// y = sky light, LINEAR.
uniform sampler2D uLightmap;

// MC sample_lightmap.glsl: uv are the light coords (0..240 each).
vec3 sampleLightmap(vec2 uv) {
    return texture(uLightmap, clamp(uv / 256.0 + 0.5 / 16.0, vec2(0.5 / 16.0), vec2(15.5 / 16.0))).rgb;
}

// Output to fragment shader
out vec2 fragTexCoord;
out vec3 fragWorldPos;
out vec4 fragColor;
flat out int fragSprite;   // see the decode below; -1 = untiled
flat out int fragRecord;   // face-mapped: texel index of the first record
flat out int fragAux;      // two-sided: back mapping (alpha byte)
flat out float fragVisibility;   // MC ChunkVisibility: the section's fade-in, 0..1
out vec3 fragLight;              // a face-mapped rectangle's lightmap colour; 1 otherwise

void main() {
    int slotRaw = int(aPosSlot.w * 65535.0 + 0.5);
    int slot = slotRaw & 0x3FF;                 // origin-table row (1024 per slab)
    bool tiled = (slotRaw & 0x8000) != 0;       // greedy-merged: tile-space uv
    bool mapped = (slotRaw & 0x4000) != 0;      // ... with per-block records in the face map
    bool twoSided = (slotRaw & 0x2000) != 0;    // tiled, drawn without culling, u mirrored on the back

    vec3 rel = aPosSlot.xyz * (65535.0 / 2048.0) - 2.0;
    // Render-space position (world minus the view's origin, exact).
    vec3 worldPos = vec3(uOrigins[slot].xyz - uRenderOrigin) + rel;
    // MC RenderSection.getVisibility: elapsed / fade, clamped; 1 with the
    // fade off or a row never stamped (0).
    int fadeStart = uOrigins[slot].w;
    fragVisibility = (uFadeMs <= 0 || fadeStart == 0)
        ? 1.0
        : clamp(float(uFadeNowMs - fadeStart) / float(uFadeMs), 0.0, 1.0);

    gl_Position = uMVP * vec4(worldPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, worldPos) + uPortalClipPlane.w
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
        } else {
            fragTexCoord = vec2(float(packedTile & 0xFF), float(packedTile >> 8));
            fragSprite = int(aTexCoord.y * 65535.0 + 0.5);
        }
    } else {
        fragTexCoord = aTexCoord;
        fragSprite = -1;
    }
    fragWorldPos = worldPos;
    fragColor = aColor;
    // MC terrain.vsh: vertexColor = Color * sample_lightmap(Sampler2, UV2).
    // A face-mapped rectangle's colour is its per-block record (the
    // fragment shader rebuilds it), so its light — one value for the whole
    // rectangle — rides fragLight instead.
    vec3 lm = sampleLightmap(aLight.rg * 255.0);
    if (mapped) {
        fragLight = lm;
    } else {
        fragColor.rgb *= lm;
        fragLight = vec3(1.0);
    }
}
