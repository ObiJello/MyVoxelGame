// File: src/server/level/ChunkKeeper.hpp
//
// Chunks a level keeps loaded and ticking without a player near them, from
// two sources:
//
//   * /forceload — MC's FORCED tickets, persisted the way vanilla stores them
//     (<dimension>/data/chunks.dat, `data.Forced` as a long array of packed
//     chunk positions), so a world Minecraft force-loaded opens the same here
//     and the other way round.
//   * The redstone index — this engine's `redstone_chunks` rule: every saved
//     chunk that holds a redstone component (IsRedstoneComponent) is kept
//     like a forced chunk, so a far-away contraption runs without a
//     simulation ring over the empty land between. The index is maintained
//     as chunks are saved and built once for an existing world by scanning
//     its region files on a background thread; it lives in
//     <dimension>/data/obeycraft_redstone_chunks.dat.
//
// Both are ticketed at ChunkLevel::ENTITY_TICKING, the level vanilla gives a
// forced chunk (31 on MC's scale), so the ticket manager's gradient makes the
// chunk itself and its eight neighbours block-ticking and the ring beyond
// that fully loaded. "Kept" is that halo (radius 2) around every source; the
// server asks it when deciding what to unload, and Service() requests the
// loads MC's tickets would have caused — in this engine tickets only tick
// chunks that something else loaded (see IntegratedServer::RequestChunkLoad).
//
// Threading: everything is server-thread except NoteChunkSaved (any thread,
// the save path) and the scan thread, which both hand their results over
// through a mutex for Service() to apply.
#pragma once

#include "server/world/ticketing/ChunkTicketManager.hpp"
#include "common/world/level/DimensionId.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace Game { class Chunk; }

namespace Server {

    // Whether any block in the chunk is a redstone component (palette walk,
    // no per-voxel pass unless a section is on the global palette).
    bool ChunkHasRedstone(const Game::Chunk& chunk);

    class ChunkKeeper {
    public:
        using ChunkPos = Game::Math::ChunkPos;
        using ChunkSet = std::unordered_set<ChunkPos, Game::Math::ChunkPosHash>;

        // dataDir / regionDir empty (an imported, read-only world): nothing is
        // persisted and no scan runs; /forceload still works for the session.
        ChunkKeeper(Game::DimensionId dimension, std::filesystem::path dataDir,
                    std::filesystem::path regionDir, bool readOnly, ChunkTicketManager* tickets);
        ~ChunkKeeper();

        ChunkKeeper(const ChunkKeeper&)            = delete;
        ChunkKeeper& operator=(const ChunkKeeper&) = delete;

        // ── /forceload ──────────────────────────────────────────────────────
        bool   AddForced(ChunkPos pos);          // false if it already was
        bool   RemoveForced(ChunkPos pos);       // false if it was not
        size_t RemoveAllForced();
        bool   IsForced(ChunkPos pos) const { return m_forced.count(pos) != 0; }
        std::vector<ChunkPos> ForcedChunks() const;

        // ── the redstone index ─────────────────────────────────────────────
        void SetRedstoneEnabled(bool on);
        bool RedstoneEnabled() const { return m_redstoneEnabled; }
        // Any thread. The chunk was just handed to the saver with this content.
        void NoteChunkSaved(ChunkPos pos, bool hasRedstone);
        size_t RedstoneChunkCount() const { return m_redstone.size(); }
        bool   ScanRunning() const { return m_scanRunning.load(std::memory_order_relaxed); }

        // ── what the server asks ───────────────────────────────────────────
        // Inside the halo of a forced or (rule on) redstone chunk: do not
        // unload, do not cancel its load.
        bool   Kept(ChunkPos pos) const { return m_kept.count(pos) != 0; }
        size_t KeptCount()   const { return m_kept.size(); }
        template <typename F> void ForEachKept(F&& f) const { for (const ChunkPos& p : m_kept) f(p); }
        size_t SourceCount() const { return m_forced.size() + (m_redstoneEnabled ? m_redstone.size() : 0); }

        // Once per tick: apply what other threads handed over, then request
        // the loads (at most `budget` new requests) for kept chunks that are
        // neither resident nor in flight.
        void Service(int64_t serverTick,
                     const std::function<bool(ChunkPos)>& isResidentOrPending,
                     const std::function<void(ChunkPos)>& requestLoad,
                     size_t budget = 64);

        // Write what changed (forced list, index). Cheap when nothing did.
        void Save();

    private:
        static constexpr int kHaloRadius = 2;   // ENTITY_TICKING +2 = FULL on the ticket gradient

        void AddTicket(ChunkPos pos, const char* identifier);
        void RemoveTicket(ChunkPos pos, const char* identifier);
        void MarkKeptDirty() { m_keptDirty = true; }
        void RebuildKept();

        bool LoadForced();
        bool SaveForced();
        bool LoadIndex();
        bool SaveIndex();
        void StartScan();
        void ScanMain();
        void ApplyIndexChange(ChunkPos pos, bool hasRedstone);

        Game::DimensionId     m_dimension;
        std::filesystem::path m_dataDir;
        std::filesystem::path m_regionDir;
        bool                  m_readOnly;
        ChunkTicketManager*   m_tickets;

        ChunkSet m_forced;
        ChunkSet m_redstone;
        ChunkSet m_kept;
        bool m_keptDirty       = true;
        bool m_forcedDirty     = false;
        bool m_indexDirty      = false;
        bool m_indexLoaded     = false;     // an index file existed (no scan needed)
        bool m_redstoneEnabled = false;

        // Cross-thread inbox: (pos, hasRedstone) from the save path and the scan.
        std::mutex m_inboxMutex;
        std::vector<std::pair<ChunkPos, bool>> m_inbox;

        std::thread       m_scanThread;
        std::atomic<bool> m_scanRunning{false};
        std::atomic<bool> m_scanStop{false};
        std::atomic<bool> m_scanFinished{false};
        bool              m_scanStarted = false;
    };

} // namespace Server
