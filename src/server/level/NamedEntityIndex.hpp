// File: src/server/level/NamedEntityIndex.hpp
//
// Every custom-named entity saved in an UNLOADED chunk, per dimension — what
// lets a `name=` selector find a named pet that is far away. A deliberate
// deviation from MC, whose selectors only ever see loaded entities:
//
//   /tp @e[name=Rex] @s          fetches Rex from wherever he was saved
//   /kill @e[type=wolf,name=Rex]  the same lookup, then the kill
//
// Only entities with a custom name are indexed; unnamed ones never are.
//
// HOW IT STAYS RIGHT. The index is rebuilt chunk by chunk from the entity
// SAVE path (LevelEntityStore::SaveChunkWith → NoteChunkSaved): each time a
// chunk's entities are written (or cleared), its entry set is replaced by
// exactly the named mobs written. A rename, a kill, a named mob walking into
// another chunk — all show up when the chunks involved are next saved, which
// always happens before they unload. Entries for chunks whose entities are
// live are never used (the selector finds those mobs directly).
//
// PERSISTENCE: <dimension>/data/obeycraft_named_entities.dat (gzip NBT, the
// SavedData shape). A world that has never had the file — an older save, an
// imported one — is scanned once on a background thread: every chunk of
// <dimension>/entities/*.mca, read through the level's chunk provider (the
// game's own AnvilChunkIo, so a concurrent save is never read half-written)
// or, for a dimension no level has opened yet, read-only straight off disk.
// Chunks saved while the scan runs keep their fresher entries.
//
// THE LOOKUP (EntitySelector): matches in unloaded chunks make the command
// wait — the chunks are held loaded (ChunkKeeper command holds), and the
// dispatcher re-runs the command once their entities are in
// (CommandDispatcher::ProcessDeferred).
//
// Threading: server thread, except the scan, which hands its results over
// through a mutex that Service() drains.
#pragma once

#include "common/core/Uuid.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Game { class Mob; }

namespace Server {

    class IntegratedServer;

    namespace NamedEntities {

        struct Entry {
            Game::Uuid           uuid{};
            std::string          name;       // the custom name's plain text
            std::string          typeSlug;   // EntityTypeInfo::slug ("wolf")
            glm::dvec3           position{0.0};
            Game::Math::ChunkPos chunk{};
        };

        // Server thread, once a tick: opens the index on first use (the
        // world's save folder), starts the scan for dimensions without an
        // index file, adopts scan results, and tells clients when the set of
        // known names changed (for `name=` completion).
        void Service(IntegratedServer& server);

        // The save path: `mobs` is everything just written for (dimension,
        // chunk). Replaces that chunk's entries with its named mobs.
        void NoteChunkSaved(Game::DimensionId dimension, Game::Math::ChunkPos chunk,
                            const std::vector<const Game::Mob*>& mobs);

        // Entries named exactly `name`, in `dimension`.
        std::vector<Entry> Find(Game::DimensionId dimension, const std::string& name);

        // Every name the index knows plus every live named mob, sorted,
        // unique — the client's `name=` completion.
        std::vector<std::string> KnownNames(IntegratedServer& server);

        // Called when KnownNames changes (Service checks once a second).
        void SetNamesChangedCallback(void (*callback)());

        // Write what changed. Cheap when nothing did.
        void Save();

        // Server stopping, before the levels go: stop and join the scan
        // (a dimension it had not finished is scanned again next time).
        void StopScan();
        // After the final save: write what changed and forget everything, so
        // the next world opened starts from its own files.
        void Close();

        // ── Holding chunks for a waiting command ────────────────────────────
        struct ChunkRef {
            Game::DimensionId    dimension = Game::DimensionId::Overworld;
            Game::Math::ChunkPos chunk{};
            bool operator==(const ChunkRef& o) const {
                return dimension == o.dimension && chunk.x == o.chunk.x && chunk.z == o.chunk.z;
            }
        };

        // Keep these chunks loaded (a ChunkKeeper command hold, creating the
        // level when needed) until Release.
        void Hold(const std::vector<ChunkRef>& chunks);
        void Release(const std::vector<ChunkRef>& chunks);
        // Every chunk is loaded and its entities have joined the level.
        bool Ready(const std::vector<ChunkRef>& chunks);
        // The chunk's entities are live (loaded and adopted), so the index
        // must not be consulted for it.
        bool EntitiesLive(Game::DimensionId dimension, Game::Math::ChunkPos chunk);

    } // namespace NamedEntities

} // namespace Server
