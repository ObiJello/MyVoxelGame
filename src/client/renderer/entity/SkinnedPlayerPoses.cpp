// File: src/client/renderer/entity/SkinnedPlayerPoses.cpp
#include "client/renderer/entity/SkinnedPlayerPoses.hpp"

#include "client/entity/Player.hpp"
#include "client/entity/PlayerSkins.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/renderer/viewmodel/HeldItemRenderer.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/ElytraAnimationState.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/PlayerArmPose.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/world/block/BedBlock.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace Render {

    namespace {

        // MC AvatarRenderer.extractFlightData's flying yaw: how far the
        // body's travel turns off its look in the horizontal plane (radians,
        // signed), when both are long enough to have a direction.
        void FlyingYRot(const glm::dvec3& movement, float yawDeg, float pitchDeg, bool& apply, float& yRot) {
            // MC Entity.calculateViewVector.
            const double f = static_cast<double>(pitchDeg) * Game::Mth::kDegToRad;
            const double g = -static_cast<double>(yawDeg) * Game::Mth::kDegToRad;
            const glm::dvec3 look(std::sin(g) * std::cos(f), -std::sin(f), std::cos(g) * std::cos(f));
            const double moveH = movement.x * movement.x + movement.z * movement.z;
            const double lookH = look.x * look.x + look.z * look.z;
            apply = moveH > 1e-5 && lookH > 1e-5;
            yRot = 0.0f;
            if (!apply) return;
            const glm::dvec2 m = glm::normalize(glm::dvec2(movement.x, movement.z));
            const glm::dvec2 l = glm::normalize(glm::dvec2(look.x, look.z));
            const double dot = m.x * l.x + m.y * l.y;
            const double sign = movement.x * look.z - movement.z * look.x;
            yRot = static_cast<float>((sign > 0.0 ? 1.0 : (sign < 0.0 ? -1.0 : 0.0)) *
                                      std::acos(std::min(1.0, std::abs(dot))));
        }

        // HumanoidMobRenderer.extractHumanoidRenderState's speedValue: a
        // fast glide damps the limb swing (the tick's travel², over 0.2,
        // cubed, at least 1).
        float GlideSpeedValue(const glm::dvec3& movement, bool fallFlying) {
            if (!fallFlying) return 1.0f;
            float v = static_cast<float>(glm::dot(movement, movement)) / 0.2f;
            v = v * v * v;
            return v < 1.0f ? 1.0f : v;
        }

        // The bed a sleeper lies in (MC bedOrientation): its FACING ordinal,
        // or -1.
        int BedFacingAt(const std::optional<glm::ivec3>& sleepingPos) {
            if (!sleepingPos || !Client::g_clientBlockAccess) return -1;
            const glm::ivec3 bed = *sleepingPos;
            const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(bed.x, bed.y, bed.z);
            if (!Game::IsBedBlock(state.Block())) return -1;
            return static_cast<int>(Game::BedFacing(state));
        }

    } // namespace

    MobRenderer::SkinnedPlayerPose LocalSkinnedPose(const Game::ClientPlayer& player,
                                                    const glm::dvec3& feet,
                                                    float headYaw, float pitch,
                                                    float pt, uint32_t playerId) {
        Client::PlayerSkins& skins = Client::PlayerSkins::Get();
        const Client::PlayerSkins::LocalBody& body = skins.LocalBodyState();
        const Client::PlayerSkins::Textures tex = skins.LocalTextures();

        MobRenderer::SkinnedPlayerPose pose;
        pose.playerId  = playerId;
        pose.position  = feet;
        pose.bodyYaw   = Game::Mth::RotLerp(pt, body.bodyYawO, body.bodyYaw);
        pose.headYaw   = headYaw;
        pose.pitch     = pitch;
        pose.walkPos   = body.walk.PositionAt(pt);
        pose.walkSpeed = body.walk.SpeedAt(pt);
        pose.ageTicks  = static_cast<float>(body.ticks) + pt;
        pose.scale     = player.physics.scale;
        pose.hurtTime  = player.hurtTime;
        pose.deathTime = player.deathTime;
        // MC Player.isCrouching: never in flight or on a seat.
        pose.crouching = player.physics.isSneaking && !player.physics.isFlying && !player.IsPassenger();
        pose.passenger = player.IsPassenger();

        // The swing (LivingEntity.getAttackAnim) and the held-item arm poses
        // (AvatarRenderer.getArmPose).
        pose.attackTime = g_heldItemRenderer.AttackAnim(pt);
        const Game::ItemStack& mainHand = player.inventory.GetSelectedStack();
        const Game::ItemStack& offHand  = player.inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN);
        const bool usingNow = player.usingItem && player.useItemRemaining > 0;
        Game::PlayerArmPose::Compute(mainHand, offHand, usingNow, player.usingHand,
                                     pose.rightArmPose, pose.leftArmPose, g_heldItemRenderer.IsSwinging());
        pose.maxCrossbowCharge = Game::PlayerArmPose::MaxCrossbowCharge(player.usingHand ? offHand : mainHand);
        pose.usingItem      = usingNow;
        pose.useItemHand    = static_cast<int>(player.usingHand ? 1 : 0);
        pose.ticksUsingItem = usingNow
            ? static_cast<float>(player.useItemDuration - player.useItemRemaining) + pt : 0.0f;
        pose.autoSpinAttack = player.IsAutoSpinAttack();
        pose.ticksSinceKineticHitFeedback = player.GetTicksSinceKineticHitFeedback(pt);

        // The glide, from the tick's travel.
        const glm::dvec3 movement = player.physics.position - skins.LocalPrevPos();
        pose.fallFlying   = player.physics.isFallFlying && !player.IsSleeping();
        pose.fallFlyTicks = pose.fallFlying ? static_cast<float>(player.fallFlyTicks) + pt : 0.0f;
        if (pose.fallFlying) FlyingYRot(movement, headYaw, pitch, pose.applyFlyingYRot, pose.flyingYRot);
        pose.speedValue = GlideSpeedValue(movement, pose.fallFlying);

        // Swimming and the cape (PlayerSkins' per-tick state).
        const Client::AvatarState& avatar = skins.LocalAvatar();
        pose.swimAmount = avatar.SwimAmount(pt);
        pose.visuallySwimming = avatar.visuallySwimming;
        pose.inWater = avatar.inWater;
        const float flyScale = std::clamp(pose.fallFlyTicks * pose.fallFlyTicks / 100.0f, 0.0f, 1.0f);
        const Client::CapePose cape = Client::ExtractCapePose(avatar, pt, skins.LocalPrevPos(),
                                                              player.physics.position, pose.bodyYaw, flyScale);
        pose.capeFlap = cape.flap;
        pose.capeLean = cape.lean;
        pose.capeLean2 = cape.lean2;

        pose.bedFacing = BedFacingAt(player.sleepingPos);

        pose.skin       = tex.skin;
        pose.slim       = tex.slim;
        pose.modelParts = tex.modelParts;
        pose.cape       = tex.cape;
        pose.elytraFlags = Game::ElytraLayerFlags(
            player.inventory.GetSlot(Game::InventoryIndexFor(Game::EquipmentSlot::CHEST)));
        pose.elytraRotX  = player.elytraAnim.RotX(pt);
        pose.elytraRotY  = player.elytraAnim.RotY(pt);
        pose.elytraRotZ  = player.elytraAnim.RotZ(pt);
        // MC renders the camera entity like any other in third person: its
        // own INVISIBILITY and GLOWING apply.
        pose.invisible = player.HasEffect(Game::MobEffectId::Invisibility);
        pose.glowing   = player.HasEffect(Game::MobEffectId::Glowing);
        pose.spectator = player.IsSpectator();

        pose.equipment[static_cast<int>(Game::EquipmentSlot::MAINHAND)] = mainHand;
        pose.equipment[static_cast<int>(Game::EquipmentSlot::OFFHAND)]  = offHand;
        for (const Game::EquipmentSlot slot : { Game::EquipmentSlot::FEET, Game::EquipmentSlot::LEGS,
                                                Game::EquipmentSlot::CHEST, Game::EquipmentSlot::HEAD }) {
            pose.equipment[static_cast<int>(slot)] = player.inventory.GetSlot(Game::InventoryIndexFor(slot));
        }
        return pose;
    }

    MobRenderer::SkinnedPlayerPose RemoteSkinnedPose(const Client::RemotePlayer& rp, float partialTick) {
        Client::PlayerSkins& skins = Client::PlayerSkins::Get();
        const Client::PlayerSkins::Textures tex = skins.RemoteTextures(rp.playerId);
        MobRenderer::SkinnedPlayerPose pose;
        pose.playerId  = rp.playerId;
        pose.position  = glm::mix(rp.renderPrevPosition, rp.position, static_cast<double>(partialTick));
        pose.bodyYaw   = Game::Mth::RotLerp(partialTick, rp.renderPrevBodyYaw, rp.bodyYaw);
        pose.headYaw   = Game::Mth::RotLerp(partialTick, rp.renderPrevRotation.x, rp.rotation.x);
        pose.pitch     = Game::Mth::Lerp(partialTick, rp.renderPrevRotation.y, rp.rotation.y);
        pose.walkPos   = rp.walk.PositionAt(partialTick);
        pose.walkSpeed = rp.walk.SpeedAt(partialTick);
        pose.ageTicks  = static_cast<float>(rp.ticks) + partialTick;
        pose.scale     = rp.scale;
        pose.hurtTime  = rp.hurtTime;
        pose.deathTime = rp.deathTime;
        pose.crouching = rp.isCrouching && rp.vehicleId == 0;
        pose.passenger = rp.vehicleId != 0;
        // The swing (PlayerSwingS2C → getAttackAnim) and the held-item arm
        // poses the server resolved (PlayerUpdateS2C).
        pose.attackTime     = rp.AttackAnim(partialTick);
        pose.rightArmPose   = rp.rightArmPose;
        pose.leftArmPose    = rp.leftArmPose;
        pose.usingItem      = rp.usingItem;
        pose.useItemHand    = rp.useItemHand;
        pose.ticksUsingItem = static_cast<float>(rp.ticksUsingItem) + (rp.usingItem ? partialTick : 0.0f);
        pose.maxCrossbowCharge = static_cast<float>(rp.maxCrossbowCharge);
        pose.autoSpinAttack = rp.autoSpinAttack;
        pose.ticksSinceKineticHitFeedback = rp.TicksSinceKineticHitFeedback(partialTick);
        // The tick's travel stands in for the delta movement a remote copy
        // does not carry.
        const glm::dvec3 movement = rp.position - rp.renderPrevPosition;
        pose.fallFlying   = rp.fallFlying && !rp.sleepingPos;
        pose.fallFlyTicks = pose.fallFlying ? static_cast<float>(rp.fallFlyTicks) + partialTick : 0.0f;
        if (pose.fallFlying) FlyingYRot(movement, pose.headYaw, pose.pitch, pose.applyFlyingYRot, pose.flyingYRot);
        pose.speedValue = GlideSpeedValue(movement, pose.fallFlying);
        if (const Client::AvatarState* avatar = skins.RemoteAvatar(rp.playerId)) {
            pose.swimAmount = avatar->SwimAmount(partialTick);
            pose.visuallySwimming = avatar->visuallySwimming;
            pose.inWater = avatar->inWater;
            const float flyScale = std::clamp(pose.fallFlyTicks * pose.fallFlyTicks / 100.0f, 0.0f, 1.0f);
            const Client::CapePose cape = Client::ExtractCapePose(*avatar, partialTick, rp.renderPrevPosition,
                                                                  rp.position, pose.bodyYaw, flyScale);
            pose.capeFlap = cape.flap;
            pose.capeLean = cape.lean;
            pose.capeLean2 = cape.lean2;
        }
        pose.bedFacing  = BedFacingAt(rp.sleepingPos);
        pose.skin       = tex.skin;
        pose.slim       = tex.slim;
        pose.modelParts = tex.modelParts;
        pose.cape       = tex.cape;
        pose.elytraFlags = rp.elytraFlags;
        pose.elytraRotX  = rp.elytraAnim.RotX(partialTick);
        pose.elytraRotY  = rp.elytraAnim.RotY(partialTick);
        pose.elytraRotZ  = rp.elytraAnim.RotZ(partialTick);
        // The synched INVISIBLE / GLOWING flags, and the spectator's head.
        pose.invisible = rp.effects.Invisible();
        pose.glowing   = rp.effects.Glowing();
        pose.spectator = rp.IsSpectator();
        if (Client::g_remotePlayerManager) {
            if (const auto* equipment = Client::g_remotePlayerManager->GetEquipment(rp.playerId)) {
                for (size_t i = 0; i < equipment->size() && i < 6; ++i) pose.equipment[i] = (*equipment)[i];
            }
        }
        return pose;
    }

} // namespace Render
