// File: src/server/entity/ShoulderEntities.hpp
//
// MC's shoulder riding, server half: ShoulderRidingEntity.setEntityOnShoulder
// and ServerPlayer's shoulderEntityLeft/Right machinery —
// setEntityOnShoulder, handleShoulderEntities (the chatter and the drop
// rules), removeEntitiesOnShoulder (the 20-tick guard) and
// respawnEntityOnShoulder — plus the two things MC gets for free from its
// entity model and this engine does not:
//
//   * the wire. MC's Player DATA_SHOULDER_PARROT_LEFT/RIGHT entity data is
//     ShoulderParrotsS2C here, broadcast on every change and sent for every
//     player to a joining client;
//   * the player file. ShoulderEntityLeft/Right are written into and read
//     from playerdata/<uuid>.dat as MC writes them (the parrot's whole
//     entity compound, "id" included).
//
// The state itself lives on ServerPlayer (getShoulderEntityLeft & co.).
// Server thread only.
#pragma once

#include <cstdint>
#include <memory>

namespace World { class NBTTagCompound; }
namespace Game {
    class Mob;
    namespace Nbt { class Writer; }
}

namespace Server {

    class ServerPlayer;
    class ServerConnection;
    class PlayerEntityView;

    namespace ShoulderEntities {

        // MC ShoulderRidingEntity.setEntityOnShoulder → ServerPlayer
        // .setEntityOnShoulder: refused while the player rides, is off the
        // ground, in water or in powder snow, or has both shoulders taken;
        // otherwise the entity's full compound goes onto the free shoulder
        // (left first), timeEntitySatOnShoulder is stamped with `gameTime`,
        // and the entity is discarded.
        bool SetEntityOnShoulder(PlayerEntityView& view, Game::Mob& entity, int64_t gameTime);

        // MC ServerPlayer.handleShoulderEntities for one player, once per
        // server tick (PlayerSession::Tick): each shoulder parrot's 1-in-200
        // chatter (or mob imitation), and the drops requested — a jump while
        // sneaking (HandlePlayerMove) or the switch to spectator. MC's other
        // drop rules (falling, water, flying, sleeping, powder snow, damage)
        // are deliberately NOT applied: the parrots stay on.
        void Tick(ServerPlayer& player);

        // The player is leaving: the riders are set down in the world (a
        // deliberate deviation — MC keeps them in the player file).
        void DropOnDisconnect(ServerPlayer& player);

        // MC ServerPlayer.removeEntitiesOnShoulder: unless the rider sat down
        // within the last 20 ticks, both shoulders' entities are respawned
        // 0.7 above the player (owned by them again) and the slots emptied.
        void RemoveEntitiesOnShoulder(ServerPlayer& player);

        // ShoulderParrotsS2C with this player's pair, to every client.
        void Broadcast(const ServerPlayer& player);

        // A joining client learns every player's pair (its own included),
        // and everyone learns the joiner's (restored from their player file).
        void SyncOnJoin(ServerConnection& joining, const ServerPlayer& joiner);

        // playerdata: MC ServerPlayer.addAdditionalSaveData /
        // readAdditionalSaveData — "ShoulderEntityLeft" / "ShoulderEntityRight"
        // compounds, written only when the shoulder is taken.
        void WritePlayerData(Game::Nbt::Writer& w, const ServerPlayer& player);
        void ReadPlayerData(const ::World::NBTTagCompound& data, ServerPlayer& player);

        // MC Player.extractParrotVariant: the Parrot.Variant id of a parrot
        // compound ("id" minecraft:parrot and a "Variant"), else -1.
        int ParrotVariantOf(const ::World::NBTTagCompound* tag);

    } // namespace ShoulderEntities

} // namespace Server
