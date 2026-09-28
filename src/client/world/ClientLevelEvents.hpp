// File: src/client/world/ClientLevelEvents.hpp
//
// MC client/renderer/LevelEventHandler.levelEvent — the client half of a
// level event — and the particle effects ClientLevel builds for it:
//
//   LevelEvent               every event's particles (bone meal's happy
//                            villagers, the dispenser's smoke, block break
//                            debris, potion splashes, the eye of ender's
//                            shatter, the spawner's flames, dragon breath,
//                            wax / scrape sparks, sculk charge, the
//                            shrieker's rings, trial spawner / vault bursts,
//                            teleport trails …), plus the sounds of the
//                            events whose sound this engine does NOT send
//                            as a sound packet of its own
//                            (Game::LevelEventSoundIsNetworked).
//   AddDestroyBlockEffect    ClientLevel.addDestroyBlockEffect: the 4x4x4
//                            (per shape box) TerrainParticle burst of a
//                            broken block.
//   AddBreakingBlockEffects  ClientLevel.addBreakingBlockEffects: the crack
//                            particle (and optionally the hit sound) on the
//                            face being mined.
//   LevelParticles           ClientPacketListener.handleParticleEvent — the
//                            expansion of a ServerLevel.sendParticles packet.
//   SpawnItemParticles       LivingEntity.spawnItemParticles: the ITEM crumbs
//                            of an item being eaten / drunk / broken, thrown
//                            from the mouth along the look.
//
// Everything runs against the ACTIVE client level (its block view and its
// particle queue) on the main thread; the jukebox's 1010 / 1011 stay with
// ClientPacketHandler (they are songs, not effects).
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Network { struct LevelParticlesS2CPacket; }

namespace Client::LevelEvents {

    void LevelEvent(int type, const glm::ivec3& pos, int data);

    void AddDestroyBlockEffect(const glm::ivec3& pos, Game::BlockState state);

    // `face` is the Direction ordinal (DOWN, UP, NORTH, SOUTH, WEST, EAST).
    void AddBreakingBlockEffects(const glm::ivec3& pos, int face, bool playSound);

    void LevelParticles(const Network::LevelParticlesS2CPacket& packet);

    // `eye` is the entity's eye position; yRot / xRot are MC's degrees.
    void SpawnItemParticles(const glm::dvec3& eye, float yRot, float xRot, uint32_t itemId, int count);

} // namespace Client::LevelEvents
