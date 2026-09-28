// File: src/client/renderer/blockentity/BlockEntityRenderers.cpp
#include "BlockEntityRenderers.hpp"
#include "BlockEntityRenderDispatcher.hpp"
#include "ChestRenderer.hpp"
#include "PistonRenderer.hpp"
#include "CampfireRenderer.hpp"
#include "BrushableBlockRenderer.hpp"
#include "DecoratedPotRenderer.hpp"
#include "BannerRenderer.hpp"
#include "BellRenderer.hpp"
#include "CopperGolemStatueRenderer.hpp"
#include "ShulkerBoxRenderer.hpp"
#include "SkullBlockRenderer.hpp"
#include "LecternRenderer.hpp"
#include "SignRenderer.hpp"
#include "HushLighthouseRenderer.hpp"
#include "AurelithHeartRenderer.hpp"
#include "VoiceBeaconRenderer.hpp"
#include "AurelithQuestRenderers.hpp"
#include "SpawnerRenderer.hpp"
#include "VaultRenderer.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/core/Log.hpp"

namespace Render {

    void RegisterAllBlockEntityRenderers() {
        if (!g_blockEntityRenderDispatcher) {
            g_blockEntityRenderDispatcher = std::make_unique<BlockEntityRenderDispatcher>();
        }

        // Chest (also serves trapped + ender, same renderer with variant
        // texture switched per blockId inside Render). Stages 6/8/9/10 will
        // add more entries here as their renderers ship.
        // Moving piston cells — MC PistonHeadRenderer.
        {
            auto piston = std::make_unique<PistonRenderer>();
            if (piston->Initialize()) {
                g_blockEntityRenderDispatcher->Register(
                    Game::BlockEntityTypeIds::PISTON, std::move(piston));
            }
        }

        auto chest = std::make_unique<ChestRenderer>();
        if (chest->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::CHEST, std::move(chest));
        } else {
            Log::Error("[BERenderers] ChestRenderer init failed");
        }

        // Shulker box — all 17 colours share one BE type, and the renderer
        // picks the entity texture per blockId inside Render, the same way the
        // chest picks normal/trapped/ender. Like the chest, its block model is
        // element-less, so without this the placed block is invisible.
        auto shulker = std::make_unique<ShulkerBoxRenderer>();
        if (shulker->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::SHULKER_BOX, std::move(shulker));
        } else {
            Log::Error("[BERenderers] ShulkerBoxRenderer init failed");
        }

        // Campfire + soul campfire. Register() takes ownership, so each type
        // id needs its own instance; they are stateless past the shader, so
        // the duplication costs one shader handle.
        for (uint16_t typeId : {Game::BlockEntityTypeIds::CAMPFIRE,
                                Game::BlockEntityTypeIds::SOUL_CAMPFIRE}) {
            auto campfire = std::make_unique<CampfireRenderer>();
            if (campfire->Initialize()) {
                g_blockEntityRenderDispatcher->Register(typeId, std::move(campfire));
            } else {
                Log::Error("[BERenderers] CampfireRenderer init failed (type %u)",
                           static_cast<unsigned>(typeId));
            }
        }

        // Skulls / mob heads — one renderer for all seven kinds, floor and
        // wall variants alike (MC registers SkullBlockRenderer once for
        // BlockEntityType.SKULL). Like the chest, the skull's block model is
        // element-less, so without this every placed skull is invisible.
        // Signs — the text on the board (the board is chunk geometry). One
        // instance per type id, as the campfire: Register takes ownership.
        for (uint16_t typeId : {Game::BlockEntityTypeIds::SIGN,
                                Game::BlockEntityTypeIds::HANGING_SIGN}) {
            auto sign = std::make_unique<SignRenderer>();
            if (sign->Initialize()) {
                g_blockEntityRenderDispatcher->Register(typeId, std::move(sign));
            } else {
                Log::Error("[BERenderers] SignRenderer init failed (type %u)", static_cast<unsigned>(typeId));
            }
        }

        auto skull = std::make_unique<SkullBlockRenderer>();
        if (skull->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::SKULL, std::move(skull));
        } else {
            Log::Error("[BERenderers] SkullBlockRenderer init failed");
        }

        // Lecterns — the open book when HAS_BOOK (MC LecternRenderer with
        // BookModel). The lectern's block model has no book of its own.
        auto lectern = std::make_unique<LecternRenderer>();
        if (lectern->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::LECTERN, std::move(lectern));
        } else {
            Log::Error("[BERenderers] LecternRenderer init failed");
        }

        // Monster spawners — the mini mob in the cage (MC SpawnerRenderer),
        // drawn through the mob renderer PlatformMain hands over once it is
        // up. The cage itself is the spawner's ordinary block model.
        g_blockEntityRenderDispatcher->Register(
            Game::BlockEntityTypeIds::MOB_SPAWNER, std::make_unique<SpawnerRenderer>());
        // Trial spawners — the same cage mob (MC TrialSpawnerRenderer), and
        // vaults — the turning display item (MC VaultRenderer, through the
        // item renderer PlatformMain hands over).
        g_blockEntityRenderDispatcher->Register(
            Game::BlockEntityTypeIds::TRIAL_SPAWNER, std::make_unique<SpawnerRenderer>());
        g_blockEntityRenderDispatcher->Register(
            Game::BlockEntityTypeIds::VAULT, std::make_unique<VaultRenderer>());

        // The Hush lighthouse lamp's sweeping beams. Its block model (lens,
        // base, cap) is ordinary chunk geometry; this draws only the light,
        // and draws it off-screen too (MC BeaconRenderer.shouldRenderOffScreen).
        auto lighthouse = std::make_unique<HushLighthouseRenderer>();
        if (lighthouse->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::HUSH_LIGHTHOUSE_LAMP, std::move(lighthouse));
        } else {
            Log::Error("[BERenderers] HushLighthouseRenderer init failed");
        }

        // Aurelith's set pieces: the Heart's hanging crystal rings over the
        // resonance engine, and the gate towers' sky beams off the voice
        // beacons. Both blocks are ordinary chunk geometry; these draw only
        // what rises above them, off-screen too.
        auto heart = std::make_unique<AurelithHeartRenderer>();
        if (heart->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::RESONANCE_ENGINE, std::move(heart));
        } else {
            Log::Error("[BERenderers] AurelithHeartRenderer init failed");
        }
        auto voiceBeacon = std::make_unique<VoiceBeaconRenderer>();
        if (voiceBeacon->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::VOICE_BEACON, std::move(voiceBeacon));
        } else {
            Log::Error("[BERenderers] VoiceBeaconRenderer init failed");
        }

        // Decorated pot — its block model has no elements, so without this
        // renderer every pot (the trial chambers' corridors are full of
        // them) is invisible.
        auto pot = std::make_unique<DecoratedPotRenderer>();
        if (pot->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::DECORATED_POT, std::move(pot));
        } else {
            Log::Error("[BERenderers] DecoratedPotRenderer init failed");
        }

        // Banners, the bell's body and copper golem statues: blocks whose
        // models are empty (or the frame only) — everything seen is theirs.
        auto banner = std::make_unique<BannerRenderer>();
        if (banner->Initialize()) {
            g_blockEntityRenderDispatcher->Register(Game::BlockEntityTypeIds::BANNER, std::move(banner));
        } else {
            Log::Error("[BERenderers] BannerRenderer init failed");
        }
        auto bell = std::make_unique<BellRenderer>();
        if (bell->Initialize()) {
            g_blockEntityRenderDispatcher->Register(Game::BlockEntityTypeIds::BELL, std::move(bell));
        } else {
            Log::Error("[BERenderers] BellRenderer init failed");
        }
        auto statue = std::make_unique<CopperGolemStatueRenderer>();
        if (statue->Initialize()) {
            g_blockEntityRenderDispatcher->Register(Game::BlockEntityTypeIds::COPPER_GOLEM_STATUE, std::move(statue));
        } else {
            Log::Error("[BERenderers] CopperGolemStatueRenderer init failed");
        }

        // Suspicious sand / gravel: the find sliding out as it is brushed
        // (the block itself is chunk mesh).
        auto brushable = std::make_unique<BrushableBlockRenderer>();
        if (brushable->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::BRUSHABLE_BLOCK, std::move(brushable));
        } else {
            Log::Error("[BERenderers] BrushableBlockRenderer init failed");
        }

        // Aurelith's quest: the key seated in a Podium socket and the item on
        // a pedestal (AurelithQuestRenderers.hpp).
        auto socket = std::make_unique<ChordSocketRenderer>();
        if (socket->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::CHORD_SOCKET, std::move(socket));
        } else {
            Log::Error("[BERenderers] ChordSocketRenderer init failed");
        }
        auto pedestal = std::make_unique<VoicePedestalRenderer>();
        if (pedestal->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::VOICE_PEDESTAL, std::move(pedestal));
        } else {
            Log::Error("[BERenderers] VoicePedestalRenderer init failed");
        }
    }

} // namespace Render
