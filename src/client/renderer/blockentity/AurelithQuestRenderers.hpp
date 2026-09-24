// File: src/client/renderer/blockentity/AurelithQuestRenderers.hpp
//
// Aurelith's quest blocks (docs/the-hush.md, "Reawakening the Heart";
// AurelithQuestBlocks.hpp): what their block models cannot show because it
// changes without the block changing.
//
//   ChordSocketRenderer    the voice key seated in a Podium socket: stood
//                          upright in the cradle's keyway, its face to the
//                          Heart, a glow of its voice's colour behind it.
//                          Dormant, a key breathes faintly; during the
//                          awakening each flares on its own note of the
//                          arpeggio (Game::Aurelith::kArpeggioStep, from the
//                          floor to the crown) and all four on the Chord;
//                          once the Chord holds them they burn steady and
//                          bright, and they gutter while the Undersong
//                          answers.
//   VoicePedestalRenderer  the item on a pedestal: turning slowly and bobbing
//                          over the ringed top, a voice key with its colour's
//                          glow beneath it (the city's hidden keys are seen
//                          from across a room).
//
// Items are drawn as the extruded sprite the held-item renderer uses
// (HeldItemSpriteMesh), with the shared block-entity shader; the glows are
// additive billboards (AurelithRenderCommon's GlowPipeline). Everything is a
// function of the level's game time and the city record (Client::
// AurelithState), so every player sees the same flare.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "AurelithRenderCommon.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/entity/Item.hpp"

#include <array>

namespace Render {

    // The shared half: the shader, a glow billboard per voice colour (and a
    // white one), the glow texture, and the sprite draw.
    class AurelithQuestRendererBase : public BlockEntityRenderer {
    public:
        ~AurelithQuestRendererBase() override;
        bool Initialize();
        void Shutdown();

    protected:
        // Draw an item stack's extruded sprite with `model` (a 0..16 pixel
        // sprite space, Y up, front at +z, already placed) at `light`.
        // False when the item has no sprite to draw.
        bool DrawItemSprite(const Game::ItemStack& stack, const glm::mat4& model, float light,
                            const glm::mat4& proj, const glm::mat4& view, const glm::vec3& cameraPos);
        // An additive glow of `voice`'s colour (-1: white) at render-space
        // `at`, `size` blocks across, at `light`.
        void DrawGlow(int voice, const glm::vec3& at, float size, float light,
                      const glm::mat4& proj, const glm::mat4& view, const glm::vec3& cameraPos);

        bool          m_initialized = false;
        ShaderHandle  m_shader = INVALID_SHADER;
        std::array<Aurelith::GpuMesh, 5> m_glow;   // voices 0..3, white 4
        TextureHandle m_glowTex = INVALID_TEXTURE;
    };

    class ChordSocketRenderer : public AurelithQuestRendererBase {
    public:
        void Render(const Game::BlockEntity& be, float partialTick,
                    const glm::mat4& proj, const glm::mat4& view,
                    const glm::vec3& cameraPos) override;
    };

    class VoicePedestalRenderer : public AurelithQuestRendererBase {
    public:
        void Render(const Game::BlockEntity& be, float partialTick,
                    const glm::mat4& proj, const glm::mat4& view,
                    const glm::vec3& cameraPos) override;
    };

} // namespace Render
