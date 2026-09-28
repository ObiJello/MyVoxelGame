// File: src/server/items/ArchaeologyItems.cpp
//
// Server half of the brush and the goat horn (common/entity/
// ArchaeologyItems.hpp): what needs the ServerPlayer's hold-to-use
// lifecycle, its cooldowns and the server level's block entities.
#include "common/entity/ArchaeologyItems.hpp"

#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Instruments.hpp"
#include "common/entity/Item.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/entity/BrushableBlockEntity.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"

#include <cmath>
#include <optional>
#include <string>

namespace Game::Archaeology {

    namespace {

        Server::ServerPlayer* AsServerPlayer(IUsePlayer& player) {
            return dynamic_cast<Server::ServerPlayer*>(&player);
        }

        // The level the player stands in.
        World* LevelOf(const Server::ServerPlayer& player) {
            Server::IntegratedServer* server = Server::g_integratedServer.get();
            if (!server) return nullptr;
            Server::ServerLevel* level = server->GetLevel(DimensionFromRaw(player.getDimensionId()));
            return level ? level->World() : nullptr;
        }

        // Direction.getUnitVec3i for a 3D data value.
        glm::ivec3 FaceStep(int face) {
            switch (face) {
                case 0: return { 0, -1,  0 };
                case 1: return { 0,  1,  0 };
                case 2: return { 0,  0, -1 };
                case 3: return { 0,  0,  1 };
                case 4: return { -1, 0,  0 };
                case 5: return { 1,  0,  0 };
                default: return { 0, 1, 0 };
            }
        }

        // BrushableBlock.getBrushSound: the suspicious blocks' own, else
        // SoundEvents.BRUSH_GENERIC.
        const char* BrushSoundFor(BlockID block) {
            switch (block) {
                case BlockID::SuspiciousSand:   return SoundEvents::BRUSH_SAND;
                case BlockID::SuspiciousGravel: return SoundEvents::BRUSH_GRAVEL;
                default:                        return SoundEvents::BRUSH_GENERIC;
            }
        }

    } // namespace

    // MC BrushItem.useOn.
    UseResult BrushUseOn(const UseOnContext& ctx, ItemStack& /*stack*/) {
        if (!ctx.world || !ctx.player) return UseResult::Consume;
        // The client's hold is its own prediction (PlayerController).
        if (ctx.world->IsClientSide()) return UseResult::Consume;
        Server::ServerPlayer* player = AsServerPlayer(*ctx.player);
        if (!player) return UseResult::Consume;
        if (ClipPlayerView(*ctx.world, *player, player->getReachDistance())) {
            player->startUsingItem(ctx.hand);
        }
        return UseResult::Consume;
    }

    // MC BrushItem.onUseTick.
    void BrushUseTick(IUsePlayer& user, ItemStack& stack, int remainingTicks) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return;
        if (remainingTicks < 0) {
            player->releaseUsingItem();
            return;
        }
        World* level = LevelOf(*player);
        if (!level) return;
        const std::optional<ViewBlockHit> hit = ClipPlayerView(*level, *player, player->getReachDistance());
        if (!hit) {
            // Looking off the block ends the hold.
            player->releaseUsingItem();
            return;
        }

        // timeElapsed = getUseDuration - ticksRemaining + 1; the stroke lands
        // on the tick before the backswing (timeElapsed % 10 == 5).
        const int timeElapsed = kBrushUseDuration - remainingTicks + 1;
        if (timeElapsed % kBrushAnimationDuration != 5) return;

        const glm::ivec3 pos = hit->pos;
        const BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);

        // BrushItem.spawnDustParticles: nextInt(7, 12) BLOCK particles of
        // the brushed state at the hit point, each flicked along the face
        // (DustParticlesDelta) by 3 × nextDouble, mirrored for a left arm.
        // MC adds them on every client from that client's own onUseTick;
        // here the server's tick sends each one exactly (count 0).
        if (state.Block() != BlockID::Air && state.Block() != BlockID::Barrier) {
            JavaRandom* random = level->Random();
            // brushingArm: the used hand's arm (the main arm is the right).
            const double flip = player->getUsedItemHand() == 0 ? 1.0 : -1.0;
            glm::dvec2 delta(0.0);   // (xd, zd)
            switch (hit->face) {
                case 0: case 1: delta = { hit->viewVector.z, -hit->viewVector.x }; break;
                case 2: delta = { 1.0, -0.1 }; break;    // NORTH
                case 3: delta = { -1.0, 0.1 }; break;    // SOUTH
                case 4: delta = { -0.1, -1.0 }; break;   // WEST
                case 5: delta = { 0.1, 1.0 }; break;     // EAST
                default: break;
            }
            const int particles = random ? random->NextInt(7, 11) : 9;
            const double x = hit->location.x - (hit->face == 4 ? 1.0e-6 : 0.0);
            const double z = hit->location.z - (hit->face == 2 ? 1.0e-6 : 0.0);
            for (int i = 0; i < particles; ++i) {
                const double vx = delta.x * flip * 3.0 * (random ? random->NextDouble() : 0.5);
                const double vz = delta.y * flip * 3.0 * (random ? random->NextDouble() : 0.5);
                level->SendParticles(ParticleOptions::Block(state), x, hit->location.y, z, 0,
                                     vx, 0.0, vz, 1.0);
            }
        }

        // level.playSound(player, pos, brushSound, BLOCKS): everyone, the
        // brusher included — its client has no stroke of its own to play it
        // from.
        level->PlaySound(nullptr, pos, BrushSoundFor(state.Block()), SoundSource::Blocks, 1.0f, 1.0f);

        if (auto* brushable = dynamic_cast<BrushableBlockEntity*>(level->GetBlockEntity(pos))) {
            const uint32_t hand = player->getUsedItemHand();
            if (brushable->Brush(*level, player, hit->face)) {
                // The finishing stroke wears the brush: hurtAndBreak(1,
                // player, the hand holding it).
                HurtAndBreak(stack, 1, level, player, hand);
            }
        }
    }

    // MC InstrumentItem.use, the server's half.
    UseResult GoatHornBegin(IUsePlayer& user, uint32_t hand, const ItemStack& stack) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return UseResult::Pass;
        const std::optional<std::string> id = stack.get(DataComponents::INSTRUMENT);
        const Instruments::Instrument* instrument = id ? Instruments::Get(*id) : nullptr;
        if (!instrument) return UseResult::Fail;

        player->startUsingItem(hand);
        // player.getCooldowns().addCooldown(stack, floor(useDuration * 20)).
        const float cooldown = instrument->useDuration;
        if (cooldown > 0.0f) {
            player->getCooldowns().AddCooldown(stack, static_cast<int>(std::floor(cooldown * 20.0f)));
        }
        return UseResult::Consume;
    }

} // namespace Game::Archaeology
