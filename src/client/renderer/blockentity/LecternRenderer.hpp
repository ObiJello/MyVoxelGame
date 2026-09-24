// File: src/client/renderer/blockentity/LecternRenderer.hpp
//
// The open book on a lectern. Mirrors MC `LecternRenderer.java` with its
// `BookModel` (client/model/object/book/BookModel.java). The lectern's block
// model has no book; MC draws it from the block entity whenever the block
// state's HAS_BOOK is true, in one fixed pose (BookModel.State.forAnimation
// (0, 0.1, 0.9, 1.2)) turned to the lectern's FACING.
//
// The book is baked once in MC's model-pixel space with every part's pose
// from BookModel.setupAnim applied; the per-lectern pose stack
// (LecternRenderer.submit) goes through the model matrix. Texture: the
// enchanting-table book, textures/entity/enchantment/enchanting_table_book.png
// (EnchantTableRenderer.BOOK_TEXTURE).
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"

#include <cstdint>

namespace Render {

    class LecternRenderer : public BlockEntityRenderer {
    public:
        LecternRenderer();
        ~LecternRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

    private:
        TextureHandle LoadTexture();

        ShaderHandle  m_shader = INVALID_SHADER;
        BufferHandle  m_vb = INVALID_BUFFER;
        BufferHandle  m_ib = INVALID_BUFFER;
        MeshHandle    m_mesh = INVALID_MESH;
        uint32_t      m_indexCount = 0;      // ONE lighting's copy
        TextureHandle m_tex = INVALID_TEXTURE;
        bool          m_texTried = false;
        int           m_packGeneration = -1; // Resources::CacheStale
        bool          m_geomBuilt = false;

        // The mesh holds one copy per lighting, back to back: MC lights the
        // book from fixed WORLD directions (EntityLighting.hpp), so the shade
        // depends on the lectern's four facings and the dimension's light
        // set — copy = facingIndex * 2 + nether.
        static constexpr int kLightingCount = 4 * 2;
    };

} // namespace Render
