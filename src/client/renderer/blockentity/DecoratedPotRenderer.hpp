// File: src/client/renderer/blockentity/DecoratedPotRenderer.hpp
//
// Draws decorated pots. Mirrors MC
// net.minecraft.client.renderer.blockentity.DecoratedPotRenderer: the pot's
// block model has no elements (particle texture only), so everything seen
// is this renderer — the neck and the top and bottom plates from
// entity/decorated_pot/decorated_pot_base (ModelLayers.DECORATED_POT_BASE,
// 32x32), and the four sides (DECORATED_POT_SIDES, 16x16) each textured by
// its sherd's pattern (<name>_pottery_sherd -> <name>_pottery_pattern) or
// decorated_pot_side for a brick / bare side.
//
// Turned to the block's facing (rotateAround Y by 180 - facing.toYRot()
// about the block centre) and wobbled the way the 26.3 renderer does it.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/entity/Item.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Render {

    class DecoratedPotRenderer : public BlockEntityRenderer {
    public:
        DecoratedPotRenderer();
        ~DecoratedPotRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

        // The pot as an item (DecoratedPotSpecialRenderer): the stack's
        // POT_DECORATIONS on its sides, unrotated (north).
        bool SupportsBEWLR() const override { return true; }
        void RenderBEWLR(Game::BlockID blockId, const glm::mat4& mvp, const BEWLRLight& light) override;
        void RenderBEWLRStack(Game::BlockID blockId, const Game::ItemStack& stack,
                              const glm::mat4& mvp, const BEWLRLight& light) override;

    private:
        void DrawItem(const std::array<Game::ItemID, 4>& sides, const glm::mat4& mvp, const BEWLRLight& light);

        // The base (neck + top + bottom, one texture) and the four side
        // planes, in PotDecorations' order: back, left, right, front.
        enum Part : int { kBase = 0, kBack, kLeft, kRight, kFront, kPartCount };

        TextureHandle LoadTexture(const std::string& stem);

        ShaderHandle  m_shader = INVALID_SHADER;
        MeshHandle    m_mesh[kPartCount]{};
        BufferHandle  m_vb[kPartCount]{};
        BufferHandle  m_ib[kPartCount]{};
        uint32_t      m_indexCount[kPartCount]{};
        bool          m_geomBuilt = false;

        std::unordered_map<std::string, TextureHandle> m_textureCache;
        int m_textureCacheGeneration = -1;   // Resources::CacheStale
    };

} // namespace Render
