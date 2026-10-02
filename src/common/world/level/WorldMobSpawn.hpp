// File: src/common/world/level/WorldMobSpawn.hpp
//
// MC EntityType.spawn(ServerLevel, …, SPAWN_ITEM_USE), reachable from common.
//
// Same shape and same reason as WorldDrops next door: spawn eggs are ordinary
// item behaviours and item behaviours live in `common`, but the mob managers
// live in `server` and common must not include server headers. One free
// function bridges it, and the include lives in the .cpp.
#pragma once

#include "common/world/level/DimensionId.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/SpawnReason.hpp"

#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <string_view>

namespace World { class NBTTagCompound; }

namespace Game {

    // Create a mob of `type` at `spawnPos` and register it with the server.
    //
    // `tryMoveDown` / `movedUp` are MC's EntityType.create arguments and are
    // what stop an egg used on the top of a block from burying the mob in it:
    // the entity is placed one block ABOVE spawnPos and then slid back down
    // until it rests on whatever is actually there. `movedUp` widens that
    // search by one block, for the case where the click already pushed the
    // spawn position up a face.
    //
    // Returns false when there is no server to spawn into, when the type is
    // unknown, or when MC's peaceful-difficulty rule rejects it — MC's own
    // `spawn(...) != null` test, which is what gates consuming the egg.
    // `dimension` is the clicked world's — an egg used in the Nether spawns
    // in the Nether (and one used through a portal spawns on the far side).
    // `portalCooldownTicks` > 0 keeps the new mob out of any portal for that
    // long (a mob spawned inside one).
    class Mob;
    // `configure` runs on the new mob after finalizeSpawn and before it joins
    // the level — MC MobBucketItem.spawn's loadFromBucketTag slot.
    // `reason` is the EntitySpawnReason finalizeSpawn sees (SPAWN_ITEM_USE
    // for an egg, BUCKET for a mob bucket).
    bool SpawnMobFromItem(EntityTypeId type, const glm::ivec3& spawnPos,
                          bool tryMoveDown, bool movedUp, DimensionId dimension,
                          int portalCooldownTicks = 0,
                          const std::function<void(Mob&)>& configure = {},
                          SpawnReason reason = SpawnReason::SpawnItemUse);

    // MC ServerLevel.anyPlayerCloseEnoughForSpawning(pos) — a non-spectator
    // player of the dimension within 128 blocks (horizontally) of the
    // centre of pos's chunk (ChunkMap.playerIsCloseEnoughForSpawning). The
    // nether portal's zombified-piglin spawn asks it. False with no server.
    bool AnyPlayerCloseEnoughForSpawning(DimensionId dimension, const glm::ivec3& pos);

    // MC Entity.saveWithoutId for a block entity that keeps a mob inside it
    // (BeehiveBlockEntity.Occupant.of): the mob's full saved compound, `id`
    // left out. Null when the mob cannot be saved.
    std::shared_ptr<::World::NBTTagCompound> SaveMobData(const Mob& mob);

    // MC EntityType.loadEntityRecursive + Level.addFreshEntity for a stored
    // mob (BeehiveBlockEntity.releaseOccupant): builds `typeSlug` ("bee") in
    // `dimension`, applies `data`, lets `place` position it, adds it to the
    // level. The new mob, or null when it could not be made or added.
    Mob* SpawnMobFromSavedData(DimensionId dimension, std::string_view typeSlug,
                               const ::World::NBTTagCompound& data,
                               const std::function<void(Mob&)>& place);

    struct ItemStack;

    // MC EnderEyeItem.use's tail (EnderEyeItem.java:85-101): locate the
    // nearest stronghold, spawn an EyeOfEnder at `from`, and point it there.
    //
    // Its own bridge rather than a call to SpawnMobFromItem because it needs
    // two things only the server can do — the stronghold lookup and
    // `EyeOfEnder::SignalTo` on the spawned entity — and doing them from the
    // item behaviour would mean common reaching into server twice.
    //
    // Returns false when there is no server, or when this world generates no
    // strongholds. MC's own `nearestMapFeature == null` branch returns CONSUME
    // WITHOUT taking the item, which is what stops an eye being wasted in a
    // world that has nowhere to send it — so the caller must not shrink the
    // stack when this answers false.
    // `thrower` (optional) is the player the USED_ENDER_EYE advancement
    // trigger is fired for, with the stronghold the eye flies to.
    class IUsePlayer;
    bool ThrowEnderEye(int dimensionId, const glm::dvec3& from, const ItemStack& stack,
                       IUsePlayer* thrower = nullptr);

    // MC EnderpearlItem.use's server half: spawn a ThrownEnderpearl owned by
    // the throwing player (shootFromRotation, power 1.5, inaccuracy 1.0).
    // Its own bridge for the eye's reason — the owner must be the player's
    // server-side entity view, which only the server can resolve. Returns
    // false with no server or no view (the caller then does not spend the
    // pearl).
    bool ThrowEnderPearl(int dimensionId, IUsePlayer& player);

    // MC ThrowablePotionItem.use's server half: spawn a thrown potion owned
    // by the throwing player carrying `stack` (count 1) —
    // Projectile.spawnProjectileFromRotation(createPotion, level, stack,
    // player, -20.0F, 0.5F, 1.0F). A lingering_potion stack makes it the
    // lingering kind. Same bridge shape and return contract as the pearl.
    bool ThrowPotion(int dimensionId, IUsePlayer& player, const ItemStack& stack);

    // MC WindChargeItem.use's server half: a WindCharge owned by the player,
    // at (x, eyeY, z) — the eye height itself, not the throwables' eye - 0.1
    // — then Projectile.spawnProjectileFromRotation(…, 0.0F, 1.5F, 1.0F).
    // Same bridge shape and return contract as the pearl.
    bool ThrowWindCharge(int dimensionId, IUsePlayer& player);

} // namespace Game
