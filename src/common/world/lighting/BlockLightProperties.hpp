// File: src/common/world/lighting/BlockLightProperties.hpp
//
// The per-BLOCK-STATE light data MC caches in BlockStateBase.initCache,
// flattened into one table over the global state id space:
//
//   emission            getLightEmission()           (Properties.lightLevel)
//   dampening           getLightDampening()          (0..15; 15 = solid)
//   canOcclude          Properties.canOcclude
//   solidRender         isSolidRender() = canOcclude && the occlusion shape
//                       is a full block
//   skyDown             propagatesSkylightDown()
//   useShape            useShapeForLightOcclusion()
//   emissiveRendering   emissiveRendering()          (faces drawn FULL_BRIGHT)
//   face occlusion      getFaceOcclusionShape(dir), for the states whose
//                       shape matters to light (!isEmptyShape)
//
// Sources: tools/gen_block_light.py → GeneratedBlockLight.inc (MC Blocks.java
// rules + class overrides, engine blocks through their slug alias), then
// EngineBlockLight.inc (hand-maintained engine glow values), evaluated per
// state against BlockRegistry's shapes and state properties.
//
// Built once by Init(), called at the end of BlockRegistry::PrewarmShapeCaches
// (the shapes are final there). Read-only afterwards: every thread may query.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <cstdint>

namespace Game::Lighting {

    struct StateLightInfo {
        uint8_t  emission  = 0;
        uint8_t  dampening = 0;
        uint8_t  flags     = 0;
        uint8_t  reserved  = 0;
        uint16_t faceSlot  = 0;     // 1-based index into the face-shape table; 0 = empty shape
        uint16_t reserved2 = 0;
    };

    class BlockLightProperties {
    public:
        static constexpr uint8_t kCanOcclude        = 1u << 0;
        static constexpr uint8_t kSolidRender       = 1u << 1;
        static constexpr uint8_t kSkyDown           = 1u << 2;
        static constexpr uint8_t kUseShape          = 1u << 3;
        static constexpr uint8_t kEmissiveRendering = 1u << 4;
        static constexpr uint8_t kHasFluid          = 1u << 5;
        // MC isCollisionShapeFullBlock — the AO shade test (0.2 vs 1.0) and
        // BlockModelLighter's faceCubic "block is full" clause.
        static constexpr uint8_t kFullCollision     = 1u << 6;
        // MC getShadeBrightness == 0.2 (the AO occluder): isCollisionShape-
        // FullBlock, with the vanilla overrides (glass family, barrier,
        // light, structure void → 1.0; mud, soul sand, full snow → 0.2).
        static constexpr uint8_t kShadeDark         = 1u << 7;

        static void Init();
        static bool Ready();

        static const StateLightInfo& Info(BlockState state);

        static int  Emission(BlockState s)     { return Info(s).emission; }
        static int  Dampening(BlockState s)    { return Info(s).dampening; }
        static bool SolidRender(BlockState s)  { return (Info(s).flags & kSolidRender) != 0; }
        static bool SkyDown(BlockState s)      { return (Info(s).flags & kSkyDown) != 0; }
        static bool UseShape(BlockState s)     { return (Info(s).flags & kUseShape) != 0; }
        static bool EmissiveRendering(BlockState s) { return (Info(s).flags & kEmissiveRendering) != 0; }
        static bool FullCollision(BlockState s) { return (Info(s).flags & kFullCollision) != 0; }
        // MC BlockState.getShadeBrightness: 0.2 or 1.0.
        static float ShadeBrightness(BlockState s) { return (Info(s).flags & kShadeDark) ? 0.2f : 1.0f; }
        // MC LightEngine.isEmptyShape: !canOcclude || !useShapeForLightOcclusion.
        static bool IsEmptyShape(BlockState s) { return Info(s).faceSlot == 0; }
        // MC BlockState.isLightPermeable: !solidRender || dampening == 0.
        static bool LightPermeable(BlockState s) {
            const StateLightInfo& i = Info(s);
            return (i.flags & kSolidRender) == 0 || i.dampening == 0;
        }
        // MC LightEngine.getOpacity: max(1, dampening) — the per-step cost.
        static int Opacity(BlockState s) {
            const int d = Info(s).dampening;
            return d > 1 ? d : 1;
        }

        // MC LightEngine.shapeOccludes(from, to, direction): does the face
        // `from` presents toward `direction`, united with the face `to`
        // presents back, close the boundary between the two cells?
        static bool ShapeOccludes(BlockState from, BlockState to, Direction direction);

        // MC ChunkSkyLightSources.isEdgeOccluded(top, bottom): does sky light
        // stop crossing from `top` down into `bottom`?
        static bool IsEdgeOccluded(BlockState top, BlockState bottom);

        // MC LightEngine.hasDifferentLightProperties — the gate on
        // LevelChunk.setBlockState's checkBlock.
        static bool HasDifferentLightProperties(BlockState oldState, BlockState newState);
    };

} // namespace Game::Lighting
