// File: src/client/renderer/blockentity/BedRenderer.hpp
//
// Draws the sixteen dyed beds — MC's BedRenderer (the entity-texture bed of
// 26.1 and earlier; 26.3 turned it into a plain block model, but the assets
// here are the 64×64 entity sheets in textures/entity/bed/, and the bed
// block model is still vanilla's element-less builtin/entity stub).
//
// ── Why this is NOT a BlockEntityRenderer ────────────────────────────────
// Same reason as EndPortalRenderer: block entities in this engine exist only
// for blocks placed through World::SetBlock, so a village bed that arrived
// inside a chunk packet has none. ClientChunkManager keeps a per-chunk index
// of bed positions (ClientChunk::beds) and this class walks that, from the
// same slot in the frame as the block-entity dispatcher.
//
// Each half draws its own piece at its own cell — MC BedRenderer.render for
// a block entity with a level: the head model for PART=HEAD, the foot model
// for PART=FOOT, posed by BedRenderer.renderPiece with translateZ = false.
// The geometry (BedRenderer.createHeadLayer / createFootLayer: one 16×16×6
// mattress cube and two 3×3×3 legs per piece) and the pose are baked once
// per (piece, facing) so a draw is a translate and nothing else.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "common/world/block/Direction.hpp"   // RenderSingle's facing
#include "BlockEntityRenderDispatcher.hpp"
#include "common/world/block/Blocks.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Client { class ClientChunkManager; struct ClientChunk; }

namespace Render {

    class BedRenderer {
    public:
        ~BedRenderer();

        // Creates the shader and bakes the eight meshes. Returns false
        // (having logged) on failure; Render() then no-ops.
        bool Initialize();
        void Shutdown();

        // Per-frame pass over every visible chunk's bed index. Called from
        // PlatformMain right after EndPortalRenderer::Render, in the main
        // pass and in every portal recursion.
        void Render(Client::ClientChunkManager* chunkMgr,
                    const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, float partialTick);

        // /morph block <bed>: a whole bed of this colour, its foot in the
        // cell at `footWorldPos` (the cell's min corner, world space) and
        // its head one cell along `facing`. No cull; the caller's.
        // `cameraWorld` is the view's eye, for the fog.
        void RenderSingle(Game::BlockID bed, Game::Direction facing, const glm::dvec3& footWorldPos,
                          const glm::mat4& projection, const glm::mat4& view,
                          const glm::dvec3& cameraWorld);

    private:
        // MC BlockEntityRenderer.getViewDistance default.
        static constexpr float kViewDistance = 64.0f;

        // One mesh per (piece, horizontal facing): head/foot × N/S/W/E.
        static constexpr int kPieces  = 2;   // 0 = head, 1 = foot
        static constexpr int kFacings = 4;   // HorizontalFacingIndex order

        struct Mesh {
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            uint32_t     indexCount = 0;
        };
        Mesh m_meshes[kPieces][kFacings];

        TextureHandle LoadColourTexture(const std::string& colour);
        // "red_bed" → "red": the entity sheet's name is the dye's.
        static std::string ColourOf(Game::BlockID id);

        bool m_initialized = false;
        ShaderHandle m_shader = INVALID_SHADER;
        std::unordered_map<std::string, TextureHandle> m_textures;
        int m_packGeneration = -1;   // Resources::CacheStale

        std::vector<Client::ClientChunk*> m_visibleChunks;
        BlockEntityRenderDispatcher::ChunkSet m_seen;
    };

    extern BedRenderer g_bedRenderer;

} // namespace Render
