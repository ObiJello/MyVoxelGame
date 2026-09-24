// File: src/common/entity/ai/village/PoiManager.hpp
//
// MC net.minecraft.world.entity.ai.village.poi.{PoiManager, PoiSection,
// PoiRecord} — the per-level index of points of interest, and the ticket
// counts that let villagers claim them.
//
// ── The shape ────────────────────────────────────────────────────────────────
// Records live per 16³ SECTION (MC SectionStorage keyed by SectionPos.asLong),
// one record per POI block, each carrying `freeTickets`: a job site starts
// with one, a bell with 32. AcquirePoi TAKES a ticket, the villager holds the
// position in a brain memory (JOB_SITE, HOME, MEETING_POINT…), and RELEASES
// it when it dies, loses the memory or gives the site up. "Is there a free
// bed" is therefore a ticket question, not a question about who is standing
// where.
//
// ── Where records come from ──────────────────────────────────────────────────
// MC keeps POI data in its own region files (poi/r.X.Z.mca) and rebuilds a
// section from its blocks whenever that data is missing or marked invalid
// (PoiManager.checkConsistencyWithBlocks). This engine does not write poi/
// files, so EVERY section takes that rebuild path: NoteChunkLoaded scans a
// chunk the first time it loads this session (a palette test rejects almost
// every section before its 4096 cells are walked), and OnBlockStateChange —
// called from World::SetBlock exactly where MC's ServerLevel.onBlockStateChange
// sits — keeps records in step with block edits. Records are not forgotten
// when a chunk unloads, for the reason NetherPortalIndex gives: forgetting
// would drop the tickets with them, and a villager whose bed chunk unloaded
// would come back to find it claimable by anyone.
//
// ── Tickets across a save ────────────────────────────────────────────────────
// Because records are rebuilt from blocks, a freshly scanned record has all of
// its tickets free. The villagers that held them still carry the positions in
// their saved brain memories (MC's own "Brain" NBT), so a villager re-claims
// its tickets when it loads (Villager::RestorePoiTickets → Restore). To make
// that idempotent — a villager whose chunk unloaded and reloaded in the same
// session must not take a second ticket — each record also remembers WHO
// holds its tickets. MC has no such list (its tickets are anonymous counts);
// the list is bookkeeping for the restore path only and never changes what
// MC's own acquire/release would decide.
//
// Server-side, server-thread only: villagers tick serially, the block hook
// runs from SetBlock, and the chunk scan from the chunk-ready pass.
#pragma once

#include "common/core/Uuid.hpp"
#include "common/entity/ai/village/PoiTypes.hpp"
#include "common/world/math/WorldMath.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Game {

    class Chunk;

    class PoiManager {
    public:
        // MC PoiManager.MAX_VILLAGE_DISTANCE — SectionTracker's level ceiling
        // (levels above 6 read as "not a village").
        static constexpr int kMaxVillageDistance = 6;

        // MC PoiManager.Occupancy.
        enum class Occupancy : uint8_t { HasSpace, IsOccupied, Any };

        using TypePredicate = std::function<bool(PoiType)>;
        using PosPredicate  = std::function<bool(const glm::ivec3&)>;

        // MC PoiRecord.
        struct Record {
            glm::ivec3        pos{0};
            PoiType           type = PoiType::Home;
            int               freeTickets = 0;
            // Who holds the taken tickets — see the header note. Never read
            // by MC's own queries.
            std::vector<Uuid> holders;

            bool HasSpace()   const { return freeTickets > 0; }
            bool IsOccupied() const { return freeTickets != PoiMaxTickets(type); }
        };

        // ── Block bookkeeping ────────────────────────────────────────────
        // MC checkConsistencyWithBlocks for every section of a chunk, the
        // first time it loads this session.
        void NoteChunkLoaded(Math::ChunkPos chunkPos, const Chunk& chunk);
        bool IsChunkScanned(int chunkX, int chunkZ) const;
        // MC ServerLevel.onBlockStateChange: the POI TYPE of the old and new
        // states are compared, not the states — a bed flipping `occupied`
        // keeps its record (and its ticket).
        void OnBlockStateChange(const glm::ivec3& pos, BlockState oldState, BlockState newState);

        // MC PoiManager.add / remove.
        bool Add(const glm::ivec3& pos, PoiType type);
        void Remove(const glm::ivec3& pos);

        // ── Lookups ──────────────────────────────────────────────────────
        std::optional<PoiType> GetType(const glm::ivec3& pos) const;
        bool Exists(const glm::ivec3& pos, const TypePredicate& predicate) const;
        const Record* GetRecord(const glm::ivec3& pos) const;

        // MC getInRange: chunks in ChunkPos.rangeClosed order (x fastest,
        // then z), sections bottom to top, then each section's records —
        // filtered by type, occupancy and the true distance
        // (distSqr <= radius²).
        std::vector<const Record*> GetInRange(const TypePredicate& predicate,
                                              const glm::ivec3& center, int radius,
                                              Occupancy occupancy) const;
        // MC getInSquare: the same walk with a Chebyshev X/Z bound.
        std::vector<const Record*> GetInSquare(const TypePredicate& predicate,
                                               const glm::ivec3& center, int radius,
                                               Occupancy occupancy) const;

        // MC findClosest(predicate, [filter,] center, radius, occupancy).
        std::optional<glm::ivec3> FindClosest(const TypePredicate& predicate,
                                              const glm::ivec3& center, int radius,
                                              Occupancy occupancy,
                                              const PosPredicate& filter = {}) const;
        // MC findAllWithType / findAllClosestFirstWithType. `filter` is MC's
        // Predicate<BlockPos>, evaluated in walk order — AcquirePoi's retry
        // cache relies on that (it marks attempts as it is asked).
        std::vector<std::pair<PoiType, glm::ivec3>>
        FindAllWithType(const TypePredicate& predicate, const PosPredicate& filter,
                        const glm::ivec3& center, int radius, Occupancy occupancy) const;
        std::vector<std::pair<PoiType, glm::ivec3>>
        FindAllClosestFirstWithType(const TypePredicate& predicate, const PosPredicate& filter,
                                    const glm::ivec3& center, int radius,
                                    Occupancy occupancy) const;

        // ── Tickets ──────────────────────────────────────────────────────
        // MC PoiManager.take: the first record in range (HAS_SPACE) passing
        // `filter` has a ticket acquired; its position is returned.
        std::optional<glm::ivec3> Take(const TypePredicate& predicate,
                                       const std::function<bool(PoiType, const glm::ivec3&)>& filter,
                                       const glm::ivec3& center, int radius,
                                       const Uuid& holder);
        // MC PoiManager.release. False when nothing was released (MC throws
        // for a position that was never registered; a stale memory here just
        // answers false).
        bool Release(const glm::ivec3& pos, const Uuid& holder);
        // Re-take the ticket a saved memory says `holder` owns — see the
        // header note. True when the holder now holds a ticket there.
        bool Restore(const glm::ivec3& pos, PoiType expected, const Uuid& holder);
        // YieldJobSite hands a claimed site to another villager with no
        // release/take in between (MC just moves the memory). The ticket's
        // bookkeeping follows it here.
        void TransferHolder(const glm::ivec3& pos, const Uuid& from, const Uuid& to);

        // ── Village distance ─────────────────────────────────────────────
        // MC PoiManager.sectionsToVillage (the DistanceTracker's level): the
        // Chebyshev distance in sections to the nearest section holding an
        // OCCUPIED village POI, capped at 7 (= none within 6).
        int SectionsToVillage(int sectionX, int sectionY, int sectionZ) const;
        int SectionsToVillage(const glm::ivec3& blockPos) const;
        // MC ServerLevel.isCloseToVillage / isVillage (distance 1).
        bool IsCloseToVillage(const glm::ivec3& blockPos, int sectionDistance) const;
        bool IsVillage(const glm::ivec3& blockPos) const { return IsCloseToVillage(blockPos, 1); }

        size_t RecordCount() const;

    private:
        struct Section {
            std::vector<Record> records;
        };
        // One loaded column: its sections by section Y, bottom to top (MC's
        // getInChunk walks minSectionY..maxSectionY in that order).
        struct Column {
            std::map<int, Section> sections;
        };

        static int64_t SectionKey(int sx, int sy, int sz);
        static int64_t SectionKeyOf(const glm::ivec3& pos);
        static int64_t ChunkKey(int cx, int cz);

        Record*       FindRecord(const glm::ivec3& pos);
        const Record* FindRecord(const glm::ivec3& pos) const;

        bool AcquireTicket(Record& record, const Uuid& holder);
        bool ReleaseTicket(Record& record, const Uuid& holder);
        // Village-centre bookkeeping (MC DistanceTracker.getLevelFromSource:
        // a section with any occupied #village record is a source).
        void NoteOccupancy(const Record& record, bool wasOccupied);
        static bool CountsForVillage(const Record& r) { return IsVillagePoi(r.type) && r.IsOccupied(); }

        void Walk(const TypePredicate& predicate, const glm::ivec3& center, int radius,
                  Occupancy occupancy, bool circular,
                  const std::function<void(const Record&)>& visit) const;

        std::unordered_map<int64_t, Column>  m_columns;
        std::unordered_set<int64_t>          m_scannedChunks;
        // Section key → number of occupied #village records in it.
        std::unordered_map<int64_t, int>     m_villageCentres;
    };

} // namespace Game
