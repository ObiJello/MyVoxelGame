// File: src/client/renderer/blockentity/PistonRenderer.cpp
#include "PistonRenderer.hpp"

#include "../entity/BlockCubeEntityRenderer.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/PistonMovingBlockEntity.hpp"

#include <algorithm>

namespace Render {

    namespace {
        Game::BlockState HeadState(bool sticky, Game::Direction facing, bool isShort) {
            Game::BlockState s = Game::BlockStates::Default(Game::BlockID::PistonHead);
            s = s.SetIndex(Game::PropertyId::PISTON_TYPE, sticky ? 1 : 0);
            s = Game::WithFacing(s, facing);
            return Game::WithBool(s, Game::PropertyId::SHORT, isShort);
        }
    }

    void PistonRenderer::Render(const Game::BlockEntity& be, float partialTick,
                                const glm::mat4&, const glm::mat4&, const glm::vec3&) {
        const auto* piston = dynamic_cast<const Game::PistonMovingBlockEntity*>(&be);
        if (!piston) return;
        Game::BlockState blockState = piston->GetMovedState();
        if (blockState.Block() == Game::BlockID::Air) return;

        // MC getProgress(partialTicks): the entity is ticked by the client
        // (ClientChunkManager::TickBlockEntities), exactly as vanilla's
        // ClientLevel ticks its own.
        const float progress = piston->GetProgress(partialTick);
        const glm::dvec3 restOrigin = glm::dvec3(piston->GetWorldPos());

        // Landed: the block is in place and the mesh is catching up. Draw
        // the carried block at rest, and nothing else (no head, no base).
        if (piston->IsLanded()) {
            g_blockCubeEntityRenderer.SubmitMovingBlock(blockState, restOrigin, piston->GetWorldPos());
            return;
        }
        // MC: `pos = blockPos.relative(movementDirection.opposite())` is the
        // cell the block is leaving — what its shading is sampled from.
        const Game::Direction movementDir = piston->GetMovementDirection();
        const glm::ivec3 aoCell = piston->GetWorldPos() -
            glm::ivec3(StepX(movementDir), StepY(movementDir), StepZ(movementDir));

        const Game::Direction direction = piston->GetDirection();
        const float extended = piston->GetExtendedProgress(progress);
        const glm::dvec3 offset(StepX(direction) * extended, StepY(direction) * extended, StepZ(direction) * extended);
        const glm::dvec3 origin = glm::dvec3(piston->GetWorldPos());

        // MC PistonHeadRenderer.extractRenderState, branch for branch.
        if (blockState.Is(Game::BlockID::PistonHead) && progress <= 4.0f) {
            blockState = Game::WithBool(blockState, Game::PropertyId::SHORT, progress <= 0.5f);
            g_blockCubeEntityRenderer.SubmitMovingBlock(blockState, origin + offset, aoCell);
        } else if (piston->IsSourcePiston() && !piston->IsExtending()) {
            const bool sticky = blockState.Is(Game::BlockID::StickyPiston);
            const Game::BlockState head = HeadState(sticky, Game::FacingOf(blockState), progress >= 0.5f);
            g_blockCubeEntityRenderer.SubmitMovingBlock(head, origin + offset, aoCell);
            // The base stays put, drawn extended (MC: basePos = pos + movement = the entity's cell).
            const Game::BlockState base = Game::WithBool(blockState, Game::PropertyId::EXTENDED, true);
            g_blockCubeEntityRenderer.SubmitMovingBlock(base, origin, piston->GetWorldPos());
        } else {
            g_blockCubeEntityRenderer.SubmitMovingBlock(blockState, origin + offset, aoCell);
        }
    }

} // namespace Render
