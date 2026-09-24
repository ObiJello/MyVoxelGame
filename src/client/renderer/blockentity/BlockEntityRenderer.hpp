// File: src/client/renderer/blockentity/BlockEntityRenderer.hpp
//
// Stage 2 of the BlockEntity system. Per-frame renderer interface for BEs
// whose geometry can't be expressed as a static block-model JSON — chest lid,
// sign text, banner patterns, beacon beam, bell clapper, etc.
//
// Mirrors MC `BlockEntityRenderer.java`. Each concrete subclass owns:
//   - shader handle
//   - texture handles per variant
//   - any per-type ModelPart rig
//
// The dispatcher walks loaded chunks each frame, frustum-culls, and invokes
// the matching renderer for every BE in range. Render runs in WORLD SPACE
// after the chunk solid pass and before portals.
#pragma once

#include "common/world/block/Blocks.hpp"
#include <glm/glm.hpp>
#include <cstdint>

namespace Game { class BlockEntity; }

namespace Render {

    // How an item-form (BEWLR) draw is lit. The block-entity shader fogs and
    // dims like the world (BlockEntityShader.hpp), so the caller says which
    // world it is in:
    //   world = true   a /morph body standing in the level — the frame's fog,
    //                  the mesh placed by `localToRender` (its model matrix
    //                  into render space) and fogged from `cameraWorld`;
    //   world = false  the viewmodel (MC lights the hand from the lightmap
    //                  but never fogs it) or a GUI icon: no fog.
    // Either way `light` is the lightmap colour the draw is multiplied by
    // (EntityEnvironment::LightColor of the holder's packed light; 1 for a
    // GUI icon).
    struct BEWLRLight {
        bool       world = false;
        glm::mat4  localToRender{1.0f};
        glm::dvec3 cameraWorld{0.0};
        glm::vec3  light{1.0f};
    };

    class BlockEntityRenderer {
    public:
        virtual ~BlockEntityRenderer() = default;

        // Per-frame draw call. `partialTick` is the [0,1] fraction within the
        // current server tick — passed through to animation lerps.
        // `cameraPos` is the world-space camera position (used by some
        // renderers for distance fade or yaw-billboarding).
        virtual void Render(const Game::BlockEntity& be,
                            float partialTick,
                            const glm::mat4& proj,
                            const glm::mat4& view,
                            const glm::vec3& cameraPos) = 0;

        // Distance cull radius. MC default is 64 blocks; renderers like the
        // beacon (which projects a beam through chunk boundaries) override
        // for larger view distance.
        virtual int  GetViewDistance() const { return 64; }

        // MC BlockEntityRenderer.shouldRenderOffScreen: the geometry reaches
        // far outside the block's own cell (the beacon's beam, the Hush
        // lighthouse's sweep), so it must draw while that cell's SECTION is
        // culled — you look away from the tower and its beam still crosses
        // the sky in front of you. MC puts such entities in LevelRenderer's
        // globalBlockEntities; here the dispatcher (a) skips the section
        // gate for them and (b) also visits the loaded chunks within
        // GetOffScreenReach() blocks (horizontally) of the camera that no
        // visible section brought in. Render is then called with nothing
        // culled but distance; the renderer bounds its own work.
        virtual bool ShouldRenderOffScreen() const { return false; }
        virtual int  GetOffScreenReach() const { return 0; }

        // BEWLR (Block Entity Without Level Renderer) hook — mirrors MC
        // `BlockEntityWithoutLevelRenderer.renderByItem`. Renders the BE's
        // model into the held-item / inventory-icon pipeline, where there's
        // no actual BlockEntity instance to draw — just an item stack. The
        // caller (HeldItemRenderer, the GUI icon path, a /morph body)
        // supplies a fully-baked model matrix + the block id so the renderer
        // can pick the right texture variant, and how the draw is lit
        // (BEWLRLight). Default: no-op (renderer doesn't support BEWLR —
        // held-item path will fall through to the cube fallback).
        virtual void RenderBEWLR(Game::BlockID /*blockId*/,
                                 const glm::mat4& /*mvp*/,
                                 const BEWLRLight& /*light*/ = BEWLRLight{}) {}

        // Whether RenderBEWLR above actually draws anything.
        //
        // The held-item path RETURNS once it hands off to a BE renderer, so a
        // renderer that inherits the no-op RenderBEWLR silently draws nothing
        // in hand — which is what happened to the campfire: it has a renderer
        // (for the fire and the cooking items) but its block geometry comes
        // from an ordinary model, so it needs the normal cube path, not this
        // one. Only a renderer that owns its item geometry should claim the
        // shortcut; everything else falls through and renders as a block.
        virtual bool SupportsBEWLR() const { return false; }
    };

} // namespace Render
