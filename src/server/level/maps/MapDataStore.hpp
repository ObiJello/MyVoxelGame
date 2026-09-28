// File: src/server/level/maps/MapDataStore.hpp
//
// The server's maps — MC's `ServerLevel.getMapData / setMapData /
// getFreeMapId`, which all go to the one DimensionDataStorage of the
// overworld (26.1 layout, the one this engine stamps its saves with):
//
//   <world>/data/idcounts.dat   {data: {map: <last id>}, DataVersion}  (MapIndex)
//   <world>/data/map_<id>.dat   {data: {dimension, xCenter, zCenter, scale,
//                                colors, trackingPosition, unlimitedTracking,
//                                locked, banners, frames}, DataVersion}
//
// Both gzip NBT, exactly as vanilla writes them, so a world carried to and
// from Minecraft keeps its maps. Maps load lazily on first use and are
// written back on autosave and shutdown when dirty (SavedData.setDirty).
//
// Also here, persisted beside them in the same folder:
//   <world>/data/obeycraft_structure_references.dat
// MC counts explorer-map claims on the structure start itself
// (StructureStart.references, in the start chunk's NBT). This engine keeps
// the terrain library's starts as opaque bytes, so the claims live in a
// small saved-data file of their own, keyed by (dimension, structure, start
// chunk) — the same identity, the same max-one-reference rule.
//
// Server thread only (the map tick, loot, trades and crafting all run
// there); a mutex still guards the tables so a stray caller cannot corrupt
// them.
#pragma once

#include "common/world/level/DimensionId.hpp"
#include "common/world/map/MapItemSavedData.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace Server {

    class MapDataStore {
    public:
        static MapDataStore& Instance();

        // A world opened: forget every map and the id counter, point at its
        // data folder. `savePath` empty or `readOnly` = maps live in memory
        // only (MC's read-only / no-save worlds).
        void Open(const std::string& savePath, bool readOnly);
        // Save what is dirty, then forget everything (world closed).
        void Close();

        // ServerLevel.getMapData: null when there is no such map.
        std::shared_ptr<Game::Maps::MapItemSavedData> Get(int32_t id);
        // ServerLevel.setMapData.
        void Set(int32_t id, std::shared_ptr<Game::Maps::MapItemSavedData> data);
        // ServerLevel.getFreeMapId → MapIndex.getNextMapId.
        int32_t GetFreeMapId();

        // Every map in memory (the carrier bookkeeping walks them when a
        // player leaves).
        void ForEachLoaded(const std::function<void(int32_t, Game::Maps::MapItemSavedData&)>& fn);

        // Write every dirty map, the id counter and the structure claims.
        void SaveAll();

        // StructureManager.addReference guarded by canBeReferenced (max one):
        // true when the start was unclaimed and now is.
        bool TryAddStructureReference(Game::DimensionId dim, const std::string& structure,
                                      int32_t chunkX, int32_t chunkZ);

    private:
        MapDataStore() = default;

        std::filesystem::path DataDir() const;
        bool Persistent() const { return !m_dataDir.empty() && !m_readOnly; }
        void LoadIndexLocked();
        void LoadReferencesLocked();
        std::shared_ptr<Game::Maps::MapItemSavedData> LoadMapLocked(int32_t id) const;
        bool SaveMapLocked(int32_t id, const Game::Maps::MapItemSavedData& data) const;
        bool SaveIndexLocked() const;
        bool SaveReferencesLocked() const;

        mutable std::mutex m_mutex;
        std::filesystem::path m_dataDir;
        bool m_readOnly = true;
        bool m_open = false;

        std::unordered_map<int32_t, std::shared_ptr<Game::Maps::MapItemSavedData>> m_maps;
        std::set<int32_t> m_missing;              // ids looked up and not on disk
        int32_t m_lastMapId = -1;                 // MapIndex.lastMapId (NO_MAP_ID = -1)
        bool m_indexLoaded = false;
        bool m_indexDirty = false;

        using ReferenceKey = std::tuple<int, std::string, int32_t, int32_t>;
        std::set<ReferenceKey> m_references;
        bool m_referencesLoaded = false;
        bool m_referencesDirty = false;
    };

} // namespace Server
