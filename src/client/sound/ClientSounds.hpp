// File: src/client/sound/ClientSounds.hpp
//
// The client's playSound calls — MC ClientLevel's sound half and the
// SimpleSoundInstance factories — for game code that is not a SoundInstance
// author.
//
//   PlayAt          ClientLevel.playSound(x, y, z, ...) (private): a
//                   positional SimpleSoundInstance; `distanceDelay` delays a
//                   sound more than 10 blocks off by distance / 40 blocks a
//                   second (thunder).
//   PlayLocal       ClientLevel.playLocalSound — the same, with a fresh seed.
//   PlayEntityBound ClientLevel.playLocalSound(Entity, ...) /
//                   playSeededSound(except, entity, ...): an
//                   EntityBoundSoundInstance that follows the entity.
//   PlayUI          SimpleSoundInstance.forUI (button clicks, toasts).
//
// Safe from any thread (SoundManager queues off-main plays).
#pragma once

#include "common/sound/SoundSource.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

namespace Game { class ClientPlayer; }

namespace Client::Sounds {

    void PlayAt(const glm::dvec3& pos, std::string_view event, Game::SoundSource source,
                float volume, float pitch, bool distanceDelay, int64_t seed);

    void PlayLocal(const glm::dvec3& pos, std::string_view event, Game::SoundSource source,
                   float volume, float pitch, bool distanceDelay = false);

    void PlayEntityBound(int32_t entityId, std::string_view event, Game::SoundSource source,
                         float volume, float pitch, int64_t seed);

    // MC SimpleSoundInstance.forUI(sound, pitch[, volume]).
    void PlayUI(std::string_view event, float pitch = 1.0f, float volume = 0.25f);

    // MC SoundEvents.UI_BUTTON_CLICK at pitch 1 — AbstractButton.playDownSound.
    inline void PlayButtonClick() { PlayUI("ui.button.click", 1.0f); }

    // The entity lookup EntityBoundSoundInstance needs: the local player
    // (whose id is its connection id), remote players and mirrored mobs.
    // Installed once by PlatformMain with the local player object.
    void InstallEntityResolver(const Game::ClientPlayer* localPlayer);

} // namespace Client::Sounds
