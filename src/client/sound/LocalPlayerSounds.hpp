// File: src/client/sound/LocalPlayerSounds.hpp
//
// The sounds the LOCAL player's own body makes, played on this client (MC
// LocalPlayer.playSound → level.playLocalSound; everyone else hears them from
// the server's replay of the move, PlayerSession::UpdateMovementStats):
//
//   • footsteps, the amethyst chime, swimming and the water-entry splash —
//     Game::PlayerMovementSounds, once per tick from the local physics;
//   • the landing of a fall that hurts — PLAYER_SMALL/BIG_FALL and the
//     landed-on block's fall sound (LivingEntity.causeFallDamage; the
//     server's damage and hurt sound follow);
//   • the nether portal's hum (LocalPlayer.handlePortalTransitionEffect →
//     PORTAL_TRIGGER when the confusion starts) and the whoosh on arriving
//     through one (level event 1032 → PORTAL_TRAVEL, played here on the
//     dimension change when the player was standing in a portal).
//
// Ticked by AmbientSounds::Tick (the unpaused world tick).
#pragma once

#include "client/sound/SoundHost.hpp"

namespace Client::LocalPlayerSounds {

    void Tick(const SoundHost::TickContext& context);
    void Reset();

    // The client just changed dimension (ChangeDimensionS2C).
    void OnDimensionChanged();

    // MC LocalPlayer.portalEffectIntensity, 0..1 (the nausea-style warp).
    float PortalEffectIntensity();

} // namespace Client::LocalPlayerSounds
