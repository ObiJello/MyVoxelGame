// File: src/client/world/ClientAnimateTick.cpp
#include "client/world/ClientAnimateTick.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/Player.hpp"
#include "common/core/Features.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockAmbientSounds.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/World.hpp"
#include "client/world/ClientLevel.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#include "common/portal/ImmersivePortal.hpp"
#include "common/world/portal/ImmersiveFrame.hpp"
#endif

#include <optional>

namespace Client {

    namespace {

        // MC ClientLevel.animateTick's loop count and its two radii. 667 x 2
        // samples per tick; the 16-block box is sampled at the same rate as the
        // 32-block one, so a cell nearby is picked roughly eight times as often
        // as one far away.
        constexpr int kIterations = 667;
        constexpr int kNearRadius = 16;
        constexpr int kFarRadius  = 32;

        // MC ClientLevel.MARKER_PARTICLE_ITEMS = Set.of(Items.BARRIER, Items.LIGHT).
        bool IsMarkerParticleItem(Game::ItemID item) {
            return item == Game::ItemRegistry::FromBlock(Game::BlockID::Barrier) ||
                   item == Game::ItemRegistry::FromBlock(Game::BlockID::Light);
        }

        // MC doAnimateTick(xt, yt, zt, r, ...): one sample, uniform in a cube
        // of side 2r centred on the camera. Note the `nextInt(r) - nextInt(r)`
        // form — that is a TRIANGULAR distribution, not uniform, so samples
        // cluster toward the camera even within one radius.
        void DoAnimateTick(const glm::ivec3& centre, int radius,
                           const Game::IBlockAccess& blocks,
                           ClientLevelBridge& sink,
                           Game::JavaRandom& random,
                           Game::BlockID markerParticleTarget) {
            const glm::ivec3 pos(
                centre.x + random.NextInt(radius) - random.NextInt(radius),
                centre.y + random.NextInt(radius) - random.NextInt(radius),
                centre.z + random.NextInt(radius) - random.NextInt(radius));

            if (pos.y < Game::World::MIN_Y || pos.y > Game::World::MAX_Y) return;

            const Game::BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
            const Game::BlockID    id    = state.Block();
            if (id == Game::BlockID::Air) return;   // the overwhelmingly common case

            const Game::Block& def = Game::BlockRegistry::Get(id);
            if (def.animateTick) def.animateTick(sink, pos, state, random);

            // MC doAnimateTick: `FluidState fluidState = blockState
            // .getFluidState(); if (!fluidState.isEmpty()) fluidState
            // .animateTick(...)` — flowing water's burble and lava's pops,
            // waterlogged blocks included.
            const Game::FluidState fluid = Game::FluidStateOf(state);
            if (!fluid.IsEmpty()) Game::FluidAnimateTickSounds(sink, pos, fluid, random);

            // MC doAnimateTick's last step: `if (markerParticleTarget ==
            // state.getBlock()) addParticle(new BlockParticleOption(
            // BLOCK_MARKER, state), x + 0.5, y + 0.5, z + 0.5, 0, 0, 0)`.
            if (markerParticleTarget != Game::BlockID::Air && markerParticleTarget == id) {
                sink.AddBlockMarkerParticle(state, static_cast<double>(pos.x) + 0.5,
                                            static_cast<double>(pos.y) + 0.5,
                                            static_cast<double>(pos.z) + 0.5);
            }
        }

#if ENABLE_IMMERSIVE_PORTALS
        // ── The Hush portal's motes on an IMMERSIVE surface ────────────────
        //
        // The vanilla Hush portal is a block (hush_portal), so the sweep above
        // reaches its animateTick (HushPortalAnimateTick) like any other
        // block's. An immersive Hush portal has no block at all — the frame's
        // interior is air with a see-through surface across it — so the
        // sweep never lands on it, and the frame would sit dark and silent.
        // This walks every HushPortal surface in the active level within the
        // sweep's far radius and runs the same per-cell body over the cells
        // the surface covers.
        //
        // Rate: the sweep picks a given cell within 16 blocks with
        // probability ≈ 2·667/32³ ≈ 4 % a tick (triangular, so more near the
        // camera; half that within 32). A flat 15 % per cell per tick is the
        // same order at close range — a surface shimmers about as a block
        // portal does — and does not fall off with distance, which is the
        // point: a portal is meant to be seen from across the city. Four
        // motes per hit, as MC spawns.
        constexpr float kImmersivePortalCellChance = 0.15f;

        // The loop body of MC NetherPortalBlock.animateTick (the same one
        // HushPortalAnimateTick ports for the block), for one cell of an
        // immersive surface. MC's neighbour test — "is the block to the west
        // or the east also a portal block?" — decides which axis the mote is
        // pushed along: across the plane (±X) when the portal runs
        // north-south, along Z otherwise. For a surface, the neighbour is a
        // portal when the cell beside it is in the frame's area.
        //
        // Vanilla portals are never horizontal; a floor frame (axis Y) falls
        // out of the same rule — pushed along Z when it is wider than one
        // block in X, along X for a one-wide strip — which is a sane look for
        // a hole in the floor and costs no special case.
        //
        // MC's 1 % ambient roll is made by the caller, for every immersive
        // portal kind that has a block counterpart with a hum.
        void AnimateImmersivePortalCell(const Game::Immersive::FrameShape& frame,
                                        const glm::ivec3& pos,
                                        ClientLevelBridge& sink,
                                        Game::JavaRandom& random) {
            const bool westIsPortal = frame.ContainsAreaCell(pos + glm::ivec3(-1, 0, 0));
            const bool eastIsPortal = frame.ContainsAreaCell(pos + glm::ivec3( 1, 0, 0));
            for (int i = 0; i < 4; ++i) {
                double x = static_cast<double>(pos.x) + random.NextDouble();
                double y = static_cast<double>(pos.y) + random.NextDouble();
                double z = static_cast<double>(pos.z) + random.NextDouble();
                double xa = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                double ya = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                double za = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                const int flip = random.NextInt(2) * 2 - 1;
                if (!westIsPortal && !eastIsPortal) {
                    x  = static_cast<double>(pos.x) + 0.5 + 0.25 * static_cast<double>(flip);
                    xa = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                } else {
                    z  = static_cast<double>(pos.z) + 0.5 + 0.25 * static_cast<double>(flip);
                    za = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                }
                sink.AddParticle(Game::ParticleKind::HushPortal, x, y, z, xa, ya, za);
            }
        }

        void AnimateImmersiveHushPortals(const glm::ivec3& cameraBlockPos,
                                         ClientLevelBridge& sink,
                                         Game::JavaRandom& random) {
            if (!ClientLevels::HasSession()) return;
            const Game::DimensionId active = ClientLevels::ActiveDimension();
            const glm::dvec3 cam = glm::dvec3(cameraBlockPos) + 0.5;
            constexpr double kFarRadiusSq = static_cast<double>(kFarRadius) * static_cast<double>(kFarRadius);

            // The bound level's store is the active level's here (main
            // thread, the sweep runs for the level the player stands in);
            // the dimension check makes that explicit rather than assumed.
            GetClientImmersivePortals().ForEach([&](const Game::Immersive::Portal& portal) {
                // The block portals' ambient hum (NetherPortalBlock
                // .animateTick's 1-in-100 PORTAL_AMBIENT and the mods' copies
                // of it): an immersive frame has no block to carry it, so
                // its surface cells do. Only the Hush draws motes here.
                const bool hush = portal.kind == Game::Immersive::PortalKind::HushPortal;
                const char* ambient = nullptr;
                switch (portal.kind) {
                    case Game::Immersive::PortalKind::HushPortal:   ambient = "obeycraft:block.hush_portal.ambient"; break;
                    case Game::Immersive::PortalKind::NetherPortal: ambient = Game::SoundEvents::PORTAL_AMBIENT; break;
                    case Game::Immersive::PortalKind::AetherPortal: ambient = "aether:block.aether_portal.ambient"; break;
                    default: break;
                }
                if (!ambient) return;
                if (portal.dimension != active) return;
                // A frame is two records facing opposite ways across the
                // same cells (MakeFlipped); emit for one of the pair or the
                // surface shimmers at twice the rate.
                if (portal.flippedPortalId != Game::Immersive::kInvalidPortalId &&
                    portal.id > portal.flippedPortalId) {
                    return;
                }
                // Within the sweep's far radius: the nearest point of the
                // surface's box to the camera.
                glm::dvec3 boxMin, boxMax;
                portal.BoundingBox(boxMin, boxMax, 0.0);
                const glm::dvec3 nearest = glm::clamp(cam, boxMin, boxMax);
                if (glm::dot(nearest - cam, nearest - cam) > kFarRadiusSq) return;

                const std::optional<Game::Immersive::FrameShape> frame =
                    Game::Immersive::FrameFromPortal(portal);
                if (!frame || frame->Empty()) return;
                for (const glm::ivec3& cell : frame->area) {
                    if (random.NextFloat() >= kImmersivePortalCellChance) continue;
                    // MC: random.nextInt(100) == 0 → playLocalSound(centre,
                    // ambient, BLOCKS, 0.5, nextFloat() * 0.4 + 0.8, false).
                    if (random.NextInt(100) == 0) {
                        sink.PlayLocalSound(glm::dvec3(cell) + glm::dvec3(0.5), ambient, Game::SoundSource::Blocks,
                                            0.5f, random.NextFloat() * 0.4f + 0.8f, false);
                    }
                    if (hush) AnimateImmersivePortalCell(*frame, cell, sink, random);
                }
            });
        }
#endif // ENABLE_IMMERSIVE_PORTALS

    } // namespace

    Game::BlockID MarkerParticleTarget(const Game::ClientPlayer& player) {
        // MC: `minecraft.gameMode.getPlayerMode() == GameType.CREATIVE`, then
        // the main-hand stack's item must be a marker item AND a BlockItem.
        if (!player.IsCreative()) return Game::BlockID::Air;
        const Game::ItemID carried = player.inventory.GetSelectedItem();
        if (IsMarkerParticleItem(carried) && Game::ItemRegistry::IsBlockItem(carried)) {
            return Game::ItemRegistry::ToBlock(carried);
        }
        return Game::BlockID::Air;
    }


    void AnimateTick(const glm::ivec3& cameraBlockPos,
                     const Game::IBlockAccess& blocks,
                     ClientLevelBridge& particleSink,
                     Game::JavaRandom& random,
                     Game::BlockID markerParticleTarget) {
        for (int i = 0; i < kIterations; ++i) {
            DoAnimateTick(cameraBlockPos, kNearRadius, blocks, particleSink, random, markerParticleTarget);
            DoAnimateTick(cameraBlockPos, kFarRadius,  blocks, particleSink, random, markerParticleTarget);
        }
#if ENABLE_IMMERSIVE_PORTALS
        // Not a block, so not something the sweep can land on: the Hush
        // portal's immersive surfaces get their motes here.
        AnimateImmersiveHushPortals(cameraBlockPos, particleSink, random);
#endif
    }

} // namespace Client
