// File: src/client/renderer/entity/SkinnedPlayerPoses.hpp
//
// Players as MC's player model draws them when their look is a Minecraft
// skin (docs/player-appearance.md): MobRenderer::SkinnedPlayerPose filled
// from a body — the walk, the swing and held-item arm poses, the glide and
// its flying yaw, the swim amount, the bed, the cape physics
// (Client::PlayerSkins' per-tick state), the skin and cape textures, what
// they wear.
//
//   LocalSkinnedPose   the ClientPlayer — for every place the local body is
//                      drawn: third person and the portal views (PlatformMain
//                      drawLocalBody) and the inventory preview.
//   RemoteSkinnedPose  a RemotePlayer, interpolated the way the stick figure
//                      is (previous tick → current).
#pragma once

#include "client/renderer/entity/MobRenderer.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Game { class ClientPlayer; }
namespace Client { struct RemotePlayer; }

namespace Render {

    // `feet` (world space) and the head's `headYaw` / `pitch` (degrees) are
    // the caller's — the drawn body's, which a portal view or the third-
    // person camera chooses; the rest is the player's own.
    MobRenderer::SkinnedPlayerPose LocalSkinnedPose(const Game::ClientPlayer& player,
                                                    const glm::dvec3& feet,
                                                    float headYaw, float pitch,
                                                    float partialTick, uint32_t playerId);

    MobRenderer::SkinnedPlayerPose RemoteSkinnedPose(const Client::RemotePlayer& player, float partialTick);

} // namespace Render
