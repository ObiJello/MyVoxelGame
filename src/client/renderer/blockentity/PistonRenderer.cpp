// File: src/client/renderer/blockentity/PistonRenderer.cpp
#include "PistonRenderer.hpp"

#include "BlockEntityRenderDispatcher.hpp"
#include "BlockEntityShader.hpp"
#include "../entity/BlockCubeEntityRenderer.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/PistonMovingBlockEntity.hpp"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace Render {

    namespace {
        Game::BlockState HeadState(bool sticky, Game::Direction facing, bool isShort) {
            Game::BlockState s = Game::BlockStates::Default(Game::BlockID::PistonHead);
            s = s.SetIndex(Game::PropertyId::PISTON_TYPE, sticky ? 1 : 0);
            s = Game::WithFacing(s, facing);
            return Game::WithBool(s, Game::PropertyId::SHORT, isShort);
        }

        // The block entity the cell carries (pistons_move_block_entities):
        // its own renderer draws it — a chest's body and lid, a sign's
        // text, a banner's cloth — shifted by the move's offset. The entity
        // sits at the moving cell, and under CarriedViewScope the level
        // answers that cell (and any other carrying cell, so a double chest
        // in flight still pairs) as the block in transit, which is what the
        // renderer reads its facing and partner from.
        //
        // `lightCell` is the cell it is lit from — the moving block's own
        // (MC MovingBlockRenderState.blockPos while in flight, the final cell
        // at rest), so the entity and the block it rides with match, and a
        // chest looks the same the frame it starts to move as the frame
        // before.
        void RenderCarried(const Game::PistonMovingBlockEntity& piston, const glm::dvec3& offset,
                           const glm::ivec3& lightCell, float partialTick, const glm::mat4& proj,
                           const glm::mat4& view, const glm::vec3& cameraPos) {
            const Game::BlockEntity* carried = piston.Carried();
            if (!carried || !g_blockEntityRenderDispatcher || !Client::g_clientBlockAccess) return;
            const Game::BlockEntityType* type = carried->GetType();
            if (!type) return;
            BlockEntityRenderer* renderer = g_blockEntityRenderDispatcher->GetRenderer(type->TypeId());
            if (!renderer) return;
            // The carried entity's own view distance, as the dispatcher would
            // apply it at rest: a chest past its renderer's reach does not
            // appear for the length of a move (this renderer reaches 68).
            {
                const glm::vec3 center = glm::vec3(carried->GetWorldPos()) + glm::vec3(0.5f);
                const float dx = center.x - cameraPos.x, dz = center.z - cameraPos.z;
                const float r = static_cast<float>(renderer->GetViewDistance());
                if (!renderer->ShouldRenderOffScreen() && dx * dx + dz * dz > r * r) return;
            }
            // Render space is world space shifted by the view's origin; the
            // move's offset is a sub-block shift on top, applied in front of
            // the renderer's own model matrix.
            const glm::mat4 shiftedView = view * glm::translate(glm::mat4(1.0f), glm::vec3(offset));
            Client::ClientBlockAccess::CarriedViewScope carriedView(*Client::g_clientBlockAccess);
            BlockEntityShader::CarriedLightScope carriedLight(carried->GetWorldPos(), lightCell);
            renderer->Render(*carried, partialTick, proj, shiftedView, cameraPos);
        }
    }

    void PistonRenderer::Render(const Game::BlockEntity& be, float partialTick,
                                const glm::mat4& proj, const glm::mat4& view, const glm::vec3& cameraPos) {
        const auto* piston = dynamic_cast<const Game::PistonMovingBlockEntity*>(&be);
        if (!piston) return;
        Game::BlockState blockState = piston->GetMovedState();
        if (blockState.Block() == Game::BlockID::Air) return;

        // MC getProgress(partialTicks): the entity is ticked by the client
        // (ClientChunkManager::TickBlockEntities), exactly as vanilla's
        // ClientLevel ticks its own.
        const float progress = piston->GetProgress(partialTick);
        const glm::dvec3 restOrigin = glm::dvec3(piston->GetWorldPos());

        // Landed: the block is in place and the section mesh is catching
        // up. Draw what the cell now holds — the state the mesh will show,
        // which can differ from the moved one (a fence joining its new
        // neighbours, a chest pairing) — at rest, lit from its own cell
        // exactly as the mesh lights it, and nothing else (no head, no
        // base). The hand-over to the mesh is then invisible.
        if (piston->IsLanded()) {
            const glm::ivec3 cell = piston->GetWorldPos();
            Game::BlockState landed = blockState;
            if (Client::g_clientBlockAccess) {
                const Game::BlockState here = Client::g_clientBlockAccess->GetBlockState(cell.x, cell.y, cell.z);
                if (here.Block() == blockState.Block()) landed = here;
            }
            g_blockCubeEntityRenderer.SubmitMovingBlock(landed, restOrigin, cell, true);
            RenderCarried(*piston, glm::dvec3(0.0), cell, partialTick, proj, view, cameraPos);
            return;
        }
        // MC: `pos = blockPos.relative(movementDirection.opposite())` is the
        // cell the block is leaving — what it is lit from while it moves.
        const Game::Direction movementDir = piston->GetMovementDirection();
        const glm::ivec3 lightCell = piston->GetWorldPos() -
            glm::ivec3(StepX(movementDir), StepY(movementDir), StepZ(movementDir));

        const Game::Direction direction = piston->GetDirection();
        const float extended = piston->GetExtendedProgress(progress);
        const glm::dvec3 offset(StepX(direction) * extended, StepY(direction) * extended, StepZ(direction) * extended);
        const glm::dvec3 origin = glm::dvec3(piston->GetWorldPos());

        // Once a carried block or an extending head has run its course it
        // rests in its final cell (offset zero) until the landing write
        // arrives: lit from there, it already looks exactly as the section
        // mesh will draw it, so the landing hand-over shows no step. In
        // flight it keeps MC's light from the cell it left.
        // The same split decides its tint: at rest, the section mesher's
        // blended tint in its final cell; in flight, MC's single biome of
        // the cell it left (MovingBlockRenderState.getBlockTint).
        const bool resting = extended == 0.0f;
        const glm::ivec3 restingLightCell = resting ? piston->GetWorldPos() : lightCell;

        // MC PistonHeadRenderer.extractRenderState, branch for branch.
        if (blockState.Is(Game::BlockID::PistonHead) && progress <= 4.0f) {
            blockState = Game::WithBool(blockState, Game::PropertyId::SHORT, progress <= 0.5f);
            g_blockCubeEntityRenderer.SubmitMovingBlock(blockState, origin + offset, restingLightCell, resting);
        } else if (piston->IsSourcePiston() && !piston->IsExtending()) {
            const bool sticky = blockState.Is(Game::BlockID::StickyPiston);
            const Game::BlockState head = HeadState(sticky, Game::FacingOf(blockState), progress >= 0.5f);
            g_blockCubeEntityRenderer.SubmitMovingBlock(head, origin + offset, lightCell, false);
            // The base stays put, drawn extended and lit from its own cell
            // (MC: basePos = pos + movement = the entity's cell) — the same
            // light and AO the section mesh gave it before the event, so the
            // hand-over from the mesh shows no step.
            const Game::BlockState base = Game::WithBool(blockState, Game::PropertyId::EXTENDED, true);
            g_blockCubeEntityRenderer.SubmitMovingBlock(base, origin, piston->GetWorldPos(), true);
        } else {
            g_blockCubeEntityRenderer.SubmitMovingBlock(blockState, origin + offset, restingLightCell, resting);
            RenderCarried(*piston, offset, restingLightCell, partialTick, proj, view, cameraPos);
        }
    }

} // namespace Render
