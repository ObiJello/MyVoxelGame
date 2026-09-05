// File: src/client/renderer/core/Vertex.hpp
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <algorithm>

namespace Render {

    // Compact block vertex: 24 bytes (down from 48).
    // Normals removed (all lighting baked into vertex color during meshing).
    // Color packed as RGBA8 (4 bytes) instead of vec4 float (16 bytes).
    struct Vertex {
        glm::vec3 pos;        // 12 bytes — world-space position
        glm::vec2 uv;         // 8 bytes  — texture atlas UV
        uint32_t packedColor;  // 4 bytes  — RGBA8 (R in low byte, A in high byte)
        // Total: 24 bytes

        Vertex() : pos(0.0f), uv(0.0f), packedColor(0xFFFFFFFF) {}

        Vertex(const glm::vec3& position, const glm::vec2& texCoord, const glm::vec4& color)
            : pos(position), uv(texCoord) {
            SetColor(color);
        }

        // Legacy constructor: accepts and ignores the normal parameter so existing
        // call sites (Mesher, FluidMeshBuilder) compile without changes.
        Vertex(const glm::vec3& position, const glm::vec3& /*normal*/,
               const glm::vec2& texCoord, const glm::vec4& vertexColor = glm::vec4(1.0f))
            : pos(position), uv(texCoord) {
            SetColor(vertexColor);
        }

        void SetColor(const glm::vec4& c) {
            auto toByte = [](float f) -> uint8_t {
                return static_cast<uint8_t>(glm::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            packedColor = toByte(c.r) | (toByte(c.g) << 8) | (toByte(c.b) << 16) | (toByte(c.a) << 24);
        }

        glm::vec4 GetColor() const {
            return glm::vec4(
                (packedColor & 0xFF) / 255.0f,
                ((packedColor >> 8) & 0xFF) / 255.0f,
                ((packedColor >> 16) & 0xFF) / 255.0f,
                ((packedColor >> 24) & 0xFF) / 255.0f
            );
        }
    };

    // TERRAIN vertex — 16 bytes. Chunk-section geometry only (Mesher,
    // FluidMeshBuilder); everything else keeps the 24-byte Vertex above.
    //
    // Why it is this small: the Metal System Trace of 2026-09-04 showed the
    // GPU vertex stage (Apple's tiler, which reads every vertex before any
    // pixel is shaded) as the frame-rate ceiling, at 0.57 ns per 32-byte
    // vertex over ~6.5 M vertices a frame — vertex FETCH bandwidth. Halving
    // the vertex halves what the tiler streams.
    //
    //   px py pz  3 x uint16   position RELATIVE TO THE SECTION ORIGIN, fixed
    //                          point: v = (p + kPosBias) * kPosScale. Range
    //                          -2..30 blocks at 1/2048 block, which covers every
    //                          model extent (MC elements reach -0.5..1.5 of the
    //                          block), the random block offset (+-0.25) and
    //                          fluid insets (0.001 rounds to 0.0005, not 0).
    //                          Integer and 1/16-block coordinates encode exactly.
    //   slot      uint16       bits 0..9: the section's row in its slab's
    //                          origin table (ChunkMegaBuffer patches this in at
    //                          upload — the mesher writes 0); bit 15: TILED;
    //                          bit 14: FACE-MAPPED (implies tiled) — a
    //                          greedy-merged rectangle whose per-block colour,
    //                          AO corners and sprite live in the FACE MAP, the
    //                          record array the mega buffer stores after the
    //                          section's vertices (see Mesher::FlushGreedyQuads
    //                          and ChunkMegaBuffer's face map).
    //   u v       2 x uint16   untiled: atlas uv as unorm16 (1/65535 of the
    //                          atlas, 0.03 px in a 2048 atlas).
    //                          tiled (fluid plates): u = tileU | tileV << 8, the
    //                          integer tile-space corner (0..16 repeats);
    //                          v = sprite id into the atlas sprite table.
    //                          face-mapped: u = tileU | tileV << 5 | tu0 << 10,
    //                          v = w | h << 5 | tv0 << 10 — the corner's tile
    //                          coordinate plus the rectangle's tile-space
    //                          origin (tu0, tv0) and size (w, h), which the
    //                          fragment shader needs to find a block's record.
    //   color     RGBA8        untiled/tiled: biome tint * AO * face shade,
    //                          alpha = tint alpha.
    //                          face-mapped: the 32-bit texel index of the
    //                          rectangle's first face-map record, RELATIVE to
    //                          the section layer's record array; the mega
    //                          buffer adds the array's slab position at upload.
    //
    // The vertex shader adds origins[slot] back (a per-slab uniform block the
    // mega buffer maintains) and decodes uv per mode; the fragment shader
    // fetches the tiled sprite's rect from the sprite table by id, and for a
    // face-mapped quad first fetches the block's record from the face map
    // (texture slot 2, a buffer texture over the slab's vertex buffer). See
    // shaders/terrain.vert and AtlasBuilder::GetSpriteTableHandle.
    struct TerrainVertex {
        uint16_t px = 0, py = 0, pz = 0;
        uint16_t slot = 0;
        uint16_t u = 0, v = 0;
        uint32_t packedColor = 0xFFFFFFFFu;
        // Total: 16 bytes

        static constexpr float    kPosScale  = 2048.0f;
        static constexpr float    kPosBias   = 2.0f;
        static constexpr uint16_t kTiledFlag    = 0x8000u;
        static constexpr uint16_t kMapFlag      = 0x4000u;
        // Tiled quad standing for a thin element's two opposite faces (cross
        // plants, seagrass): its four vertices are indexed with both windings
        // and the fragment shader mirrors the texture in u on the geometric
        // back, decided from the quad's normal against the camera — see
        // Mesher::EmitTwoSided.
        static constexpr uint16_t kTwoSidedFlag = 0x2000u;
        static constexpr uint16_t kFlagMask     = 0xE000u;
        static constexpr uint16_t kSlotMask  = 0x03FFu;

        static uint16_t EncodeUnorm16(float f) {
            return static_cast<uint16_t>(glm::clamp(f, 0.0f, 1.0f) * 65535.0f + 0.5f);
        }
        static uint16_t EncodePos(float rel) {
            const float v = (rel + kPosBias) * kPosScale + 0.5f;
            return static_cast<uint16_t>(glm::clamp(v, 0.0f, 65535.0f));
        }
        static float DecodePos(uint16_t v) {
            return static_cast<float>(v) / kPosScale - kPosBias;
        }

        // An unmerged quad corner: world-space Vertex from the mesher, made
        // relative to the section origin `base` (world block coordinates of
        // the section's min corner).
        static TerrainVertex FromWorld(const Vertex& src, const glm::ivec3& base) {
            TerrainVertex t;
            t.px = EncodePos(src.pos.x - static_cast<float>(base.x));
            t.py = EncodePos(src.pos.y - static_cast<float>(base.y));
            t.pz = EncodePos(src.pos.z - static_cast<float>(base.z));
            t.slot = 0;
            t.u = EncodeUnorm16(src.uv.x);
            t.v = EncodeUnorm16(src.uv.y);
            t.packedColor = src.packedColor;
            return t;
        }

        // A greedy-merged FLUID plate corner: section-relative position,
        // integer tile-space uv (0..16 repeats each) and the sprite to tile.
        // Colour is uniform over the plate (fluids carry no AO).
        static TerrainVertex Tiled(const glm::vec3& rel, int tileU, int tileV,
                                   uint16_t spriteId, uint32_t color) {
            TerrainVertex t;
            t.px = EncodePos(rel.x);
            t.py = EncodePos(rel.y);
            t.pz = EncodePos(rel.z);
            t.slot = kTiledFlag;
            t.u = static_cast<uint16_t>((tileU & 0xFF) | ((tileV & 0xFF) << 8));
            t.v = spriteId;
            t.packedColor = color;
            return t;
        }

        // A two-sided quad corner: the FRONT face's uv within its sprite in
        // sixteenths (0..16 each, so a model face's uv sub-rect encodes
        // exactly), its sprite, the front normal as a 6-bit code (two bits
        // per axis: 0 = zero, 1 = +, 3 = -; every non-zero component has
        // the same magnitude — axis-aligned or 45-degree planes only), and
        // in the colour's alpha byte how the BACK face's mapping relates to
        // the front's: bits 0..5 = the mirrored extent's sum (u0 + u1 or
        // v0 + v1) in sixteenths (0..32), bits 6..7 = mode (0: u' = sum - u,
        // side faces; 1: v' = sum - v, top/bottom faces; 2: identical, a
        // model that flipped its own uv). u = tileU | tileV << 5 |
        // code << 10. The fragment shader applies the mapping when the
        // camera is behind the normal and outputs alpha 1.
        static TerrainVertex TwoSided(const glm::vec3& rel, int tileU16, int tileV16, int normalCode,
                                      int mirrorMode, int mirrorSum16, uint16_t spriteId, uint32_t color) {
            TerrainVertex t = Tiled(rel, 0, 0, spriteId, color);
            t.slot = static_cast<uint16_t>(kTiledFlag | kTwoSidedFlag);
            t.u = static_cast<uint16_t>((tileU16 & 0x1F) | ((tileV16 & 0x1F) << 5) | ((normalCode & 0x3F) << 10));
            t.packedColor = (color & 0x00FFFFFFu) |
                            (static_cast<uint32_t>((mirrorSum16 & 0x3F) | ((mirrorMode & 3) << 6)) << 24);
            return t;
        }

        // A greedy-merged BLOCK rectangle corner (face-mapped): the corner's
        // tile coordinate (0..16), the rectangle's tile-space origin (tu0,
        // tv0, 0..15) and size (w, h, 1..16), and the texel index of its
        // first face-map record within the section layer's record array.
        static TerrainVertex Mapped(const glm::vec3& rel, int tileU, int tileV,
                                    int tu0, int tv0, int w, int h, uint32_t recordTexel) {
            TerrainVertex t;
            t.px = EncodePos(rel.x);
            t.py = EncodePos(rel.y);
            t.pz = EncodePos(rel.z);
            t.slot = static_cast<uint16_t>(kTiledFlag | kMapFlag);
            t.u = static_cast<uint16_t>((tileU & 0x1F) | ((tileV & 0x1F) << 5) | ((tu0 & 0xF) << 10));
            t.v = static_cast<uint16_t>((w & 0x1F) | ((h & 0x1F) << 5) | ((tv0 & 0xF) << 10));
            t.packedColor = recordTexel;
            return t;
        }
        // One face-map record = two uint32 words = one RGBA16 texel of the
        // slab's buffer texture (r16 = colour r | g << 8, g16 = colour b |
        // AO byte << 8, b16 = sprite id, a16 = 0): word 0 = tint * face
        // shade in rgb with the block's four 2-bit AO corner codes in the
        // top byte (tile-corner order, see Mesher::TryStashGreedyQuad);
        // word 1 = the sprite id. The vertex's record index counts records.
        static constexpr uint32_t kFaceMapWordsPerRecord = 2;
        static uint32_t FaceMapTexel0(uint32_t baseColor, uint8_t aoByte) {
            return (baseColor & 0x00FFFFFFu) | (static_cast<uint32_t>(aoByte) << 24);
        }
        static uint32_t FaceMapTexel1(uint16_t spriteId) {
            return static_cast<uint32_t>(spriteId);
        }
    };

    static_assert(sizeof(TerrainVertex) == 16,
                  "TerrainVertex must match GetTerrainVertexLayout / ChunkMegaBuffer::VERTEX_STRIDE");

} // namespace Render
