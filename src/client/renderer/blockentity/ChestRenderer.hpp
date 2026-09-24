// File: src/client/renderer/blockentity/ChestRenderer.hpp
//
// In-world chest renderer. Builds the MC-equivalent chest geometry (base +
// lid + lock) and draws it depth-tested into the world. Texture is the chest
// entity atlas (assets/textures/entity/chest/{normal,trapped,ender}.png) —
// loaded per-variant on first use.
//
// Lid open animation will arrive in Stage 4 when ContainerOpenersCounter
// + BlockEntityActionS2C are wired; until then the lid is drawn closed.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/world/block/Blocks.hpp"
#include <unordered_map>
#include <string>

namespace Render {

    class ChestRenderer : public BlockEntityRenderer {
    public:
        ChestRenderer();
        ~ChestRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

        void RenderBEWLR(Game::BlockID blockId, const glm::mat4& mvp,
                         const BEWLRLight& light = BEWLRLight{}) override;
        // Owns its item geometry — see BlockEntityRenderer::SupportsBEWLR.
        bool SupportsBEWLR() const override { return true; }

    private:
        TextureHandle LoadVariantTexture(const std::string& variant);
        const char*   VariantForBlock(Game::BlockID id) const;

        // MC ChestModel has THREE layer definitions — single, double-left and
        // double-right — and ChestRenderer picks one per chest from its
        // ChestType. Same here: one mesh per variant, chosen at draw time.
        enum Variant { kSingle = 0, kLeft = 1, kRight = 2, kVariantCount = 3 };

        // Each variant is two meshes — the BODY (bottom box) and the LID (lid
        // and lock, MC's two parts that share the hinge at (0, 9, 1)) — so the
        // lid can swing on its own pose. Each mesh holds one copy of its
        // geometry per lighting, back to back; a draw picks one with
        // DrawIndexed's index offset.
        //
        // MC lights the chest from fixed WORLD directions (EntityLighting.hpp),
        // so the shade depends on the facing, the dimension's light set and,
        // for the lid, how far it has swung. Copy 0 is the item form (BEWLR,
        // the block-model face table every block icon uses). Then the body has
        // 4 facings × 2 light sets; the lid the same, each at kLidAngleSteps+1
        // hinge angles (closed .. 90°). A step is 5.6°, under 2% of shade —
        // invisible, where a per-frame vertex upload would be a buffer write
        // per chest per frame.
        enum Part { kBody = 0, kLid = 1, kPartCount = 2 };
        static constexpr int kItemLighting  = 0;
        static constexpr int kLidAngleSteps = 16;
        static int BodyLighting(int facing, bool nether) {
            return 1 + facing * 2 + (nether ? 1 : 0);
        }
        static int LidLighting(int facing, bool nether, int angleStep) {
            return 1 + (facing * 2 + (nether ? 1 : 0)) * (kLidAngleSteps + 1) + angleStep;
        }
        static constexpr int kLightingCount[kPartCount] = {
            1 + 4 * 2,
            1 + 4 * 2 * (kLidAngleSteps + 1),
        };

        void DrawPart(Variant variant, Part part, int lighting);

        ShaderHandle  m_shader = INVALID_SHADER;
        MeshHandle    m_mesh[kVariantCount][kPartCount] = {};
        BufferHandle  m_vb[kVariantCount][kPartCount]   = {};
        BufferHandle  m_ib[kVariantCount][kPartCount]   = {};
        std::unordered_map<std::string, TextureHandle> m_textureCache;
        int m_textureCacheGeneration = -1;   // Resources::CacheStale

        bool          m_geomBuilt    = false;
        uint32_t      m_indexCount[kVariantCount][kPartCount] = {};   // ONE lighting's copy
    };

} // namespace Render
