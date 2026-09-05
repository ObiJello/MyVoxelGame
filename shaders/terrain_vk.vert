// File: shaders/terrain_vk.vert (Vulkan twin of terrain.vert)
// Chunk-TERRAIN vertex shader for the packed 16-byte TerrainVertex — see
// terrain.vert for the attribute encoding. ChunkRenderer registers
// GetTerrainVertexLayout() on the three terrain shaders so their pipelines
// bake the 16-byte input; the shared block_vk.vert must NOT be pointed at
// terrain buffers (entity renderers feed it 24-byte vertices).
//
// SectionOrigins is descriptor set 3 of the portal pipeline layout — the
// backend's user uniform block (RenderBackend::BindUniformBuffer), bound by
// ChunkMegaBuffer::BindSlab with the slab's 16 KB window as dynamic offset.
#version 450

layout (location = 0) in vec4 aPosSlot;   // px py pz slot (R16G16B16A16_UNORM)
layout (location = 1) in vec2 aTexCoord;  // u v (R16G16_UNORM)
layout (location = 2) in vec4 aColor;     // RGBA8 normalized

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

layout (std140, set = 3, binding = 0) uniform SectionOrigins {
    vec4 uOrigins[1024];
};

// Output to fragment shader
layout (location = 0) out vec2 fragTexCoord;
layout (location = 1) out vec3 fragWorldPos;
layout (location = 2) out vec4 fragColor;
layout (location = 3) flat out int fragSprite;   // see the decode below; -1 = untiled
layout (location = 4) flat out int fragRecord;   // face-mapped: texel index of the first record
layout (location = 5) flat out int fragAux;      // two-sided: back mapping (alpha byte)

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
    vec3 worldPos = uOrigins[slot].xyz + rel;

    gl_Position = pc.uMVP * vec4(worldPos, 1.0);
    // Portal-plane clipping — same contract and rationale as block_vk.vert.
    gl_ClipDistance[0] = (any(notEqual(pc.uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(pc.uPortalClipPlane.xyz, worldPos) + pc.uPortalClipPlane.w
        : 1.0;

    // fragSprite, per mode (all flat, so the fragment shader branches per
    // primitive):
    //   untiled       -1
    //   tiled (fluid) sprite id (bits 0..15); bit 17 = two-sided, bits
    //                 19..24 = its front-normal code; fragAux = the back
    //                 mapping (TerrainVertex::TwoSided's alpha byte)
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
            fragSprite = int(aTexCoord.y * 65535.0 + 0.5) | 0x20000 | (((packedTile >> 10) & 0x3F) << 19);
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
}
