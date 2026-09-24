// File: src/common/sound/LevelSound.hpp
//
// The shared half of MC's Level.playSound family — the `except` argument, the
// seed, and the server's broadcast seam.
//
// THE CALL SHAPE
//   MC: level.playSound(@Nullable Entity except, x, y, z, SoundEvent, SoundSource,
//                       volume, pitch)
//   Here: level.PlaySound(except, pos, "event.id", SoundSource, volume, pitch)
// on both Game::ILevelWrite (blocks, item behaviours) and Game::EntityLevel
// (entities). Each side implements it the way MC's two Level classes do:
//
//   ServerLevel.playSeededSound  → every player in the dimension within
//       SoundEvent.getRange(volume) EXCEPT `except` gets a ClientboundSoundPacket
//       (PlayerList.broadcast). Only a Player is ever excepted: a mob passed as
//       `except` is ignored, exactly as ServerLevel's `except instanceof Player`.
//   ClientLevel.playSeededSound  → plays locally ONLY when `except` is the local
//       player — i.e. when this client is predicting its own player's action
//       and the server will send the sound to everyone else. With a null
//       `except` the client stays silent and waits for the packet.
//
// That pairing is the whole of "the acting player hears it at once, everyone
// else hears it from the server": the same common behaviour runs on the client
// (prediction) and the server (authority), both pass the acting player, and
// each side does its half.
//
// `except` is MC's Entity, and this engine has two kinds of player object: the
// item-behaviour player (Game::IUsePlayer — ServerPlayer on the server,
// ClientUsePlayer on the client) and the entity-system view of a player
// (Game::Entity with IsPlayer(), the server's PlayerEntityView). SoundExcept
// accepts either, or nullptr.
#pragma once

#include "common/sound/SoundSource.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Game {

    class IUsePlayer;
    class Entity;

    struct SoundExcept {
        const IUsePlayer* player = nullptr;
        const Entity*     entity = nullptr;

        constexpr SoundExcept() = default;
        constexpr SoundExcept(std::nullptr_t) {}
        constexpr SoundExcept(const IUsePlayer* p) : player(p) {}
        constexpr SoundExcept(const Entity* e) : entity(e) {}

        constexpr bool IsNone() const { return player == nullptr && entity == nullptr; }
    };

    namespace Sound {

        // MC Level.soundSeedGenerator.nextLong(): the seed a played sound
        // carries, from which the client's SoundInstance random picks the
        // weighted variant (so every client hears the SAME footstep). Thread-
        // safe; any thread may play a sound.
        int64_t NextSeed();

        // The server's broadcast (MC ServerLevel.playSeededSound →
        // PlayerList.broadcast). Common code cannot see sessions or
        // connections, so the server installs one of these at startup and
        // Game::World and the server's entity bridge forward to it. Absent (a
        // client-only process, a unit test), a server-side play is dropped.
        //
        // Callable from any thread: the implementation sends at once on the
        // server thread and queues from anywhere else (the falling-block and
        // explosion work runs on worker threads).
        class ServerSoundSink {
        public:
            virtual ~ServerSoundSink() = default;

            // MC ClientboundSoundPacket to every player in `dimension` within
            // range of `pos`, minus `except`.
            virtual void PlaySound(DimensionId dimension, const SoundExcept& except,
                                   const glm::dvec3& pos, std::string_view event,
                                   SoundSource source, float volume, float pitch,
                                   int64_t seed) = 0;

            // MC ClientboundSoundEntityPacket: the sound follows entity
            // `entityId` on the client (EntityBoundSoundInstance). `pos` is the
            // entity's position now, for the range test.
            virtual void PlaySoundFromEntity(DimensionId dimension, const SoundExcept& except,
                                             int32_t entityId, const glm::dvec3& pos,
                                             std::string_view event, SoundSource source,
                                             float volume, float pitch, int64_t seed) = 0;
        };

        void SetServerSink(ServerSoundSink* sink);
        ServerSoundSink* GetServerSink();

        // The block-centre convenience every BlockPos overload of MC's
        // Level.playSound applies: pos + 0.5 on each axis.
        inline glm::dvec3 BlockCenter(const glm::ivec3& pos) {
            return glm::dvec3(pos) + glm::dvec3(0.5);
        }

    } // namespace Sound

} // namespace Game
