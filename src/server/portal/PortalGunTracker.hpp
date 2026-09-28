// File: src/server/portal/PortalGunTracker.hpp
//
// Keeps every portal-gun pair tied to the life of its gun. A pair belongs to
// one gun stack (the PORTAL_GUN_INSTANCE_ID component); when that stack is
// gone — burned, blown up, despawned, fallen into the void, deleted in the
// creative inventory, cleared by /clear, consumed in an anvil — the pair
// closes with the normal close burst (PortalRegistry::ClearPair).
//
// THE RULE. A pair closes only on proof, never on absence of knowledge:
//
//   • Each pair remembers where its gun was last confirmed (GunWhereabouts,
//     saved with the pair in data/portal_gun.json): a player's inventory, a
//     dropped item, a block container, an entity (item frame, armor stand,
//     mob), or "stored" on disk with an unloaded chunk.
//   • Every half second the tracker re-checks that location. A LIVE location
//     the server can inspect (an online player, a loaded chunk) that no
//     longer holds the gun triggers a census of everything live: every online
//     player's inventory, cursor and open menu; every dropped item; every
//     block container in every loaded chunk; every entity (through its saved
//     NBT, so any holder that persists is seen). Found → the whereabouts move
//     there. Not found on two consecutive checks → the gun is gone and the
//     pair closes.
//   • A dropped gun that is DESTROYED (emptied without a player, hopper or
//     mob collecting it) closes its pair at the next check if the census
//     cannot find another copy — no second strike needed.
//   • An offline player, an unloaded chunk or an unknown location can never
//     be inspected, so they never close a pair: a gun in a chest far away or
//     in a logged-out friend's inventory keeps its portals.
//
// ONE-TIME ORPHAN SWEEP. Pairs whose gun was destroyed before this tracker
// existed have no whereabouts, so the rule above can never close them. Once
// per world (data/portal_gun.json "orphanSweep"), a background thread reads
// EVERY saved chunk (region/ and entities/ of every dimension, through the
// game's own AnvilChunkIo so a concurrent save is never read half-written)
// and every .dat file (level.dat, playerdata/ — i.e. offline players'
// inventories and ender chests), and byte-scans the decompressed NBT for the
// gun-id tag ItemStackNbt writes. It needs no NBT tree and finds a gun at any
// depth. Meanwhile the live census keeps running; every gun seen live while
// the sweep runs, or by one last census when it ends, counts as existing. A
// pair whose gun was found nowhere closes. An unreadable chunk makes the
// sweep inconclusive: nothing closes and it runs again on the next load.
// /portalgun sweep re-runs it on demand.
//
// DUPLICATE IDS. A creative middle-click clone (ContainerInput::CLONE) and a
// creative-palette stack that arrives carrying an id are NEW guns: the id is
// stripped server-side (StripGunInstance) and the copy mints its own on its
// first shot, owning its own pair. Copies that already exist from older
// saves are harmless — a pair closes only when no copy is found anywhere.
//
// THREADING. Everything here is server-thread only except the sweep's own
// worker, which touches nothing but its job and read-only region I/O.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Game { struct ItemStack; }
namespace Server { class IntegratedServer; }

namespace Game::Portal {

    // The gun instance id of `stack` itself; 0 when it is not a portal gun or
    // has never fired (no id assigned yet).
    uint64_t GunInstanceOf(const Game::ItemStack& stack);

    // Every assigned gun id in `stack`, nested bundle contents included.
    void ForEachGunIn(const Game::ItemStack& stack, const std::function<void(uint64_t)>& fn);

    // Remove the instance id from a portal-gun stack (the duplicate policy
    // above). True when an id was removed.
    bool StripGunInstance(Game::ItemStack& stack);

    // Every gun id in an encoded (uncompressed) NBT buffer — the named
    // TAG_Long "obeycraft:portal_gun_instance_id" ItemStackNbt writes.
    void ScanNbtForGunIds(const uint8_t* data, size_t size,
                          const std::function<void(uint64_t)>& fn);

    // What became of a dropped gun as it left its ItemEntityManager.
    enum class GunItemFate : uint8_t {
        Destroyed,   // emptied without anyone collecting it
        Collected,   // a player, hopper or mob took it
        Stored,      // its chunk unloaded and was saved
    };
    void NoteGunItemEntity(uint64_t gunId, GunItemFate fate, Game::DimensionId dimension,
                           const glm::dvec3& pos);

    // Once per server tick (always, frozen or not: a creative delete happens
    // while the world is paused too).
    void TickGunTracker(Server::IntegratedServer& server);

    // A player finished joining: their inventory is loaded, re-check soon.
    void NoteGunTrackerPlayerJoined(const std::string& playerName);

    // Server stopping: cancel and join the sweep, forget the session state.
    // Call after the server thread has joined, before the levels go.
    void ShutdownGunTracker();

    // ── /portalgun ─────────────────────────────────────────────────────────
    enum class SweepStart : uint8_t { Started, AlreadyRunning, NothingToCheck, NoSaveFolder };
    // `requesterPlayerId` gets the result in chat (0 = the automatic sweep).
    SweepStart StartOrphanSweep(uint32_t requesterPlayerId);
    bool OrphanSweepRunning();

    // Guns whose pair was fired by `playerName`, or whose gun that player
    // is carrying now (case-insensitive).
    std::vector<uint64_t> GunsOfPlayer(const std::string& playerName);

    // "in Steve's inventory", "dropped at 10 64 -3 (overworld)", ...
    std::string DescribeWhereabouts(uint64_t gunId);

} // namespace Game::Portal

#endif // ENABLE_PORTAL_GUN
