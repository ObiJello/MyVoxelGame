// File: src/client/renderer/blockentity/PistonRenderer.hpp
//
// MC PistonHeadRenderer — draws the block a piston is carrying (and, for a
// retracting piston, its head and base) offset by the move's progress.
//
// Rendering goes through BlockCubeEntityRenderer::SubmitMovingBlock, the
// same instanced block-model path falling blocks and primed TNT use, so a
// moving block is exactly the geometry the chunk mesher would have drawn
// for it, just translated. MC's renderer likewise hands the block model to
// the ordinary block renderer (submitMovingBlock).
//
// The client does not tick block entities, so the two-tick animation is
// clocked from the moment the entity arrived over the wire.
#pragma once

#include "BlockEntityRenderer.hpp"

namespace Render {

    class PistonRenderer : public BlockEntityRenderer {
    public:
        bool Initialize() { return true; }

        void Render(const Game::BlockEntity& be, float partialTick,
                    const glm::mat4& proj, const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

        // MC PistonHeadRenderer.getViewDistance.
        int GetViewDistance() const override { return 68; }
    };

} // namespace Render
