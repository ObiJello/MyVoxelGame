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

    // Chunk-TERRAIN vertex: the 24-byte block vertex plus an 8-byte sprite
    // tile rect, 32 bytes. Terrain-only — every non-terrain renderer (entities,
    // block entities, GUI, portals, …) keeps `Vertex` and the 24-byte
    // GetBlockVertexLayout untouched.
    //
    // The tile rect exists for greedy meshing. A merged quad covers several
    // blocks with ONE quad, so its texture must REPEAT per block — which a
    // texture atlas cannot do with plain wrap modes. The mesher therefore
    // writes the quad's uv in TILE space (0..N block repeats) and rides the
    // sprite's full atlas rect on the vertex; the terrain fragment shaders
    // fold it back per fragment:
    //     atlasUV = tileOrigin + fract(uv) * tileSize
    // sampled with textureGrad on the UNWRAPPED uv's derivatives (scaled by
    // tileSize) so mip selection matches an unmerged quad exactly.
    //
    // tileSize == (0,0) is the "no tiling" flag: uv is then a direct atlas
    // coordinate and the shader samples it untouched, keeping every unmerged
    // quad bit-identical to the old 24-byte path. The rect is unorm16
    // (value = x/65535): the smallest sprite (16px in a <=4096px atlas) is
    // ~256 units wide, so a real rect can never quantize to the zero flag.
    struct TerrainVertex {
        glm::vec3 pos;         // 12 bytes — world-space position       (offset 0)
        glm::vec2 uv;          // 8 bytes  — atlas UV, or tile-space UV (offset 12)
        uint32_t packedColor;  // 4 bytes  — RGBA8                      (offset 20)
        uint16_t tileOriginU = 0;  // sprite min U, unorm16             (offset 24)
        uint16_t tileOriginV = 0;  // sprite min V, unorm16             (offset 26)
        uint16_t tileSizeU = 0;    // sprite U extent, unorm16; 0 = untiled (offset 28)
        uint16_t tileSizeV = 0;    // sprite V extent, unorm16          (offset 30)
        // Total: 32 bytes

        TerrainVertex() : pos(0.0f), uv(0.0f), packedColor(0xFFFFFFFF) {}

        // Implicit on purpose: the mesher and fluid builder construct plain
        // `Vertex` quads and push them into TerrainVertex vectors; this is the
        // conversion that keeps those call sites unchanged (tile fields stay
        // zero, i.e. "sample uv as-is").
        TerrainVertex(const Vertex& v)
            : pos(v.pos), uv(v.uv), packedColor(v.packedColor) {}

        static uint16_t EncodeUnorm16(float f) {
            return static_cast<uint16_t>(glm::clamp(f, 0.0f, 1.0f) * 65535.0f + 0.5f);
        }
    };

    static_assert(sizeof(TerrainVertex) == 32,
                  "TerrainVertex must match GetTerrainVertexLayout / ChunkMegaBuffer::VERTEX_STRIDE");

} // namespace Render
