// File: src/client/renderer/blockentity/SkullBlockRenderer.hpp
//
// In-world skull / mob-head renderer. Mirrors MC `SkullBlockRenderer.java` +
// the models under `client/model/object/skull/` (SkullModel, PiglinHeadModel,
// DragonHeadModel). The skull BLOCK model is empty in vanilla — everything a
// placed skull shows comes from this block-entity renderer.
//
// One static mesh per skull kind, baked once in MC's model-pixel space
// (y-down, exactly as authored); the per-cell pose — floor offset or
// against-the-wall offset, plus the 16-segment rotation — is applied through
// the model matrix each draw, following MC submitSkull()'s pose stack.
//
// Textures are the MOBS' own entity sheets (SkullBlockRenderer.SKIN_BY_TYPE):
// entity/skeleton/skeleton.png and friends, loaded lazily per kind.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"
#include <array>
#include <cstdint>

namespace Render {

    class SkullBlockRenderer : public BlockEntityRenderer {
    public:
        SkullBlockRenderer();
        ~SkullBlockRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

    private:
        // MC SkullBlock.Types, same order.
        enum Kind : int {
            kSkeleton = 0,
            kWitherSkeleton,
            kPlayer,
            kZombie,
            kCreeper,
            kPiglin,
            kDragon,
            kKindCount,
        };

        // BlockID -> (kind, wall?). kind < 0 means "not a skull block".
        static void ClassifyBlock(Game::BlockID id, int& outKind, bool& outWall);

        TextureHandle LoadKindTexture(int kind);

        ShaderHandle m_shader = INVALID_SHADER;
        bool         m_geomBuilt = false;

        std::array<BufferHandle,  kKindCount> m_vb{};
        std::array<BufferHandle,  kKindCount> m_ib{};
        std::array<MeshHandle,    kKindCount> m_mesh{};
        std::array<uint32_t,      kKindCount> m_indexCount{};
        std::array<TextureHandle, kKindCount> m_tex{};
        std::array<bool,          kKindCount> m_texTried{};
    };

} // namespace Render
