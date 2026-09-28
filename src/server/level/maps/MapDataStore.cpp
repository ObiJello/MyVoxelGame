// File: src/server/level/maps/MapDataStore.cpp
#include "MapDataStore.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace Server {

    namespace {

        using Game::Maps::MapItemSavedData;

        constexpr const char* kIndexFile      = "idcounts.dat";
        constexpr const char* kReferencesFile = "obeycraft_structure_references.dat";

        std::string MapFileName(int32_t id) { return "map_" + std::to_string(id) + ".dat"; }

        bool ReadGzipNbt(const std::filesystem::path& file, std::shared_ptr<::World::NBTTagCompound>& root) {
            std::ifstream f(file, std::ios::binary);
            if (!f) return false;
            const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (raw.empty()) return false;
            std::vector<uint8_t> nbt;
            if (!Game::Nbt::GzipDecompress(raw, nbt)) return false;
            root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
            return root != nullptr;
        }

        // SavedData files are written whole to a temporary and renamed over
        // the old one, as DimensionDataStorage.tryWrite does.
        bool WriteGzipFile(const std::filesystem::path& file, const std::vector<uint8_t>& nbt) {
            std::vector<uint8_t> gz;
            if (!Game::Nbt::GzipCompress(nbt, gz)) return false;
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            const std::filesystem::path tmp = file.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) return false;
                f.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
                if (!f) return false;
            }
            std::filesystem::rename(tmp, file, ec);
            return !ec;
        }

        // BlockPos.CODEC is an int array [x, y, z]; pre-1.20.5 files used a
        // {X, Y, Z} compound, which vanilla's datafixer upgrades — read both.
        bool ReadBlockPos(const ::World::NBTTagCompound& c, const std::string& key, glm::ivec3& out) {
            auto tag = c.GetTag(key);
            if (auto arr = std::dynamic_pointer_cast<::World::NBTTagIntArray>(tag); arr && arr->value.size() >= 3) {
                out = glm::ivec3(arr->value[0], arr->value[1], arr->value[2]);
                return true;
            }
            if (auto comp = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag)) {
                out = glm::ivec3(comp->GetValue<int32_t>("X", 0), comp->GetValue<int32_t>("Y", 0),
                                 comp->GetValue<int32_t>("Z", 0));
                return true;
            }
            return false;
        }

        std::optional<std::string> ReadPlainText(const ::World::NBTTagCompound& c, const std::string& key) {
            if (auto str = std::dynamic_pointer_cast<::World::NBTTagString>(c.GetTag(key))) return str->value;
            if (auto comp = std::dynamic_pointer_cast<::World::NBTTagCompound>(c.GetTag(key))) {
                if (auto text = std::dynamic_pointer_cast<::World::NBTTagString>(comp->GetTag("text"))) return text->value;
            }
            return std::nullopt;
        }

        Game::DimensionId ReadDimension(const ::World::NBTTagCompound& data) {
            auto tag = data.GetTag("dimension");
            if (auto str = std::dynamic_pointer_cast<::World::NBTTagString>(tag)) {
                if (auto d = Game::DimensionFromRegistryName(str->value)) return *d;
                return Game::DimensionId::Overworld;
            }
            // Pre-1.16 maps stored the dimension as its int id.
            if (auto i = std::dynamic_pointer_cast<::World::NBTTagInt>(tag)) return Game::DimensionFromRaw(i->value);
            if (auto b = std::dynamic_pointer_cast<::World::NBTTagByte>(tag)) return Game::DimensionFromRaw(b->value);
            return Game::DimensionId::Overworld;
        }

        bool ReadBool(const ::World::NBTTagCompound& c, const std::string& key, bool fallback) {
            if (!c.HasTag(key)) return fallback;
            return c.GetValue<int8_t>(key, fallback ? 1 : 0) != 0;
        }
    }

    MapDataStore& MapDataStore::Instance() {
        static MapDataStore store;
        return store;
    }

    void MapDataStore::Open(const std::string& savePath, bool readOnly) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_maps.clear();
        m_missing.clear();
        m_references.clear();
        m_lastMapId = -1;
        m_indexLoaded = m_indexDirty = false;
        m_referencesLoaded = m_referencesDirty = false;
        m_readOnly = readOnly;
        m_dataDir.clear();
        m_open = true;
        if (savePath.empty()) return;
        std::string reason;
        if (auto root = Game::Anvil::SaveRoot::Open(savePath, reason)) {
            m_dataDir = root->DataDir(Game::DimensionId::Overworld);
        } else {
            Log::Warning("[Maps] no world folder for '%s' (%s); maps will not be saved", savePath.c_str(),
                         reason.c_str());
        }
    }

    void MapDataStore::Close() {
        SaveAll();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_maps.clear();
        m_missing.clear();
        m_references.clear();
        m_lastMapId = -1;
        m_indexLoaded = m_indexDirty = false;
        m_referencesLoaded = m_referencesDirty = false;
        m_dataDir.clear();
        m_open = false;
    }

    std::filesystem::path MapDataStore::DataDir() const { return m_dataDir; }

    // ── Maps ────────────────────────────────────────────────────────────

    std::shared_ptr<MapItemSavedData> MapDataStore::Get(int32_t id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_maps.find(id); it != m_maps.end()) return it->second;
        if (m_missing.count(id) || m_dataDir.empty()) return nullptr;
        auto loaded = LoadMapLocked(id);
        if (!loaded) {
            m_missing.insert(id);
            return nullptr;
        }
        m_maps[id] = loaded;
        return loaded;
    }

    void MapDataStore::Set(int32_t id, std::shared_ptr<MapItemSavedData> data) {
        if (!data) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        data->SetDirty();   // DimensionDataStorage.set marks the new data dirty
        m_missing.erase(id);
        m_maps[id] = std::move(data);
    }

    int32_t MapDataStore::GetFreeMapId() {
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadIndexLocked();
        // Never hand out an id a map already in memory uses (a world whose
        // idcounts.dat went missing still has its map_<n>.dat files).
        do {
            ++m_lastMapId;
        } while (m_maps.count(m_lastMapId) != 0 ||
                 (!m_dataDir.empty() && std::filesystem::exists(m_dataDir / MapFileName(m_lastMapId))));
        m_indexDirty = true;
        return m_lastMapId;
    }

    void MapDataStore::ForEachLoaded(const std::function<void(int32_t, MapItemSavedData&)>& fn) {
        std::vector<std::pair<int32_t, std::shared_ptr<MapItemSavedData>>> copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            copy.assign(m_maps.begin(), m_maps.end());
        }
        for (auto& [id, data] : copy) fn(id, *data);
    }

    void MapDataStore::LoadIndexLocked() {
        if (m_indexLoaded) return;
        m_indexLoaded = true;
        if (m_dataDir.empty()) return;
        std::shared_ptr<::World::NBTTagCompound> root;
        if (!ReadGzipNbt(m_dataDir / kIndexFile, root)) return;
        auto data = std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data"));
        if (!data) return;
        // MapIndex.CODEC: optionalFieldOf("map", -1).
        m_lastMapId = data->GetValue<int32_t>("map", -1);
    }

    std::shared_ptr<MapItemSavedData> MapDataStore::LoadMapLocked(int32_t id) const {
        std::shared_ptr<::World::NBTTagCompound> root;
        const std::filesystem::path file = m_dataDir / MapFileName(id);
        if (!std::filesystem::exists(file)) return nullptr;
        if (!ReadGzipNbt(file, root)) {
            Log::Error("[Maps] could not read %s", file.string().c_str());
            return nullptr;
        }
        auto data = std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data"));
        if (!data) return nullptr;

        auto map = std::make_shared<MapItemSavedData>(
            data->GetValue<int32_t>("xCenter", 0), data->GetValue<int32_t>("zCenter", 0),
            static_cast<int8_t>(std::clamp<int>(data->GetValue<int8_t>("scale", 0), 0, Game::Maps::kMaxScale)),
            ReadBool(*data, "trackingPosition", true), ReadBool(*data, "unlimitedTracking", false),
            ReadBool(*data, "locked", false), ReadDimension(*data));
        if (auto colors = std::dynamic_pointer_cast<::World::NBTTagByteArray>(data->GetTag("colors"));
            colors && colors->value.size() == map->colors.size()) {
            for (size_t i = 0; i < map->colors.size(); ++i) map->colors[i] = static_cast<uint8_t>(colors->value[i]);
        }
        if (auto banners = std::dynamic_pointer_cast<::World::NBTTagList>(data->GetTag("banners"))) {
            for (const auto& elem : banners->value) {
                auto b = std::dynamic_pointer_cast<::World::NBTTagCompound>(elem);
                if (!b) continue;
                Game::Maps::MapBanner banner;
                if (!ReadBlockPos(*b, "pos", banner.pos) && !ReadBlockPos(*b, "Pos", banner.pos)) continue;
                banner.color = b->GetValue<std::string>("color", b->GetValue<std::string>("Color", "white"));
                banner.name = ReadPlainText(*b, "name");
                if (!banner.name) banner.name = ReadPlainText(*b, "Name");
                map->MutableBanners()[banner.Id()] = banner;
            }
        }
        if (auto frames = std::dynamic_pointer_cast<::World::NBTTagList>(data->GetTag("frames"))) {
            for (const auto& elem : frames->value) {
                auto f = std::dynamic_pointer_cast<::World::NBTTagCompound>(elem);
                if (!f) continue;
                Game::Maps::MapFrame frame;
                if (!ReadBlockPos(*f, "pos", frame.pos) && !ReadBlockPos(*f, "Pos", frame.pos)) continue;
                frame.rotation = f->HasTag("rotation") ? f->GetValue<int32_t>("rotation", 0)
                                                       : f->GetValue<int32_t>("Rotation", 0);
                frame.entityId = f->HasTag("entity_id") ? f->GetValue<int32_t>("entity_id", 0)
                                                        : f->GetValue<int32_t>("EntityId", 0);
                map->MutableFrames()[frame.Id()] = frame;
            }
        }
        map->RestoreMarkersAfterLoad();
        map->ClearDirty();
        return map;
    }

    bool MapDataStore::SaveMapLocked(int32_t id, const MapItemSavedData& map) const {
        Game::Nbt::Writer w;
        w.BeginRootCompound();
        w.BeginCompound("data");
        w.String("dimension", Game::DimensionRegistryName(map.dimension));
        w.Int("xCenter", map.centerX);
        w.Int("zCenter", map.centerZ);
        w.Byte("scale", map.scale);
        w.ByteArray("colors", reinterpret_cast<const int8_t*>(map.colors.data()), map.colors.size());
        w.Bool("trackingPosition", map.trackingPosition);
        w.Bool("unlimitedTracking", map.unlimitedTracking);
        w.Bool("locked", map.locked);
        {
            auto list = w.BeginList("banners", Game::Nbt::TagType::Compound);
            for (const auto& [key, banner] : map.Banners()) {
                w.ListCompoundBegin(list);
                const int32_t pos[3] = {banner.pos.x, banner.pos.y, banner.pos.z};
                w.IntArray("pos", pos, 3);
                w.String("color", banner.color);
                if (banner.name) w.String("name", *banner.name);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        {
            auto list = w.BeginList("frames", Game::Nbt::TagType::Compound);
            for (const auto& [key, frame] : map.Frames()) {
                w.ListCompoundBegin(list);
                const int32_t pos[3] = {frame.pos.x, frame.pos.y, frame.pos.z};
                w.IntArray("pos", pos, 3);
                w.Int("rotation", frame.rotation);
                w.Int("entity_id", frame.entityId);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        w.EndCompound();
        w.Int("DataVersion", Game::Save::DataVersion());
        w.EndRootCompound();
        if (!w.ok()) return false;
        return WriteGzipFile(m_dataDir / MapFileName(id), w.Bytes());
    }

    bool MapDataStore::SaveIndexLocked() const {
        Game::Nbt::Writer w;
        w.BeginRootCompound();
        w.BeginCompound("data");
        w.Int("map", m_lastMapId);
        w.EndCompound();
        w.Int("DataVersion", Game::Save::DataVersion());
        w.EndRootCompound();
        if (!w.ok()) return false;
        return WriteGzipFile(m_dataDir / kIndexFile, w.Bytes());
    }

    // ── Structure references ────────────────────────────────────────────

    void MapDataStore::LoadReferencesLocked() {
        if (m_referencesLoaded) return;
        m_referencesLoaded = true;
        if (m_dataDir.empty()) return;
        std::shared_ptr<::World::NBTTagCompound> root;
        if (!ReadGzipNbt(m_dataDir / kReferencesFile, root)) return;
        auto data = std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data"));
        if (!data) return;
        // {data: {<dimension>: [{structure, chunk: [x, z]}, …]}}
        for (const auto& [dimName, tag] : data->value) {
            const auto dim = Game::DimensionFromRegistryName(dimName);
            auto list = std::dynamic_pointer_cast<::World::NBTTagList>(tag);
            if (!dim || !list) continue;
            for (const auto& elem : list->value) {
                auto c = std::dynamic_pointer_cast<::World::NBTTagCompound>(elem);
                if (!c) continue;
                auto chunk = std::dynamic_pointer_cast<::World::NBTTagIntArray>(c->GetTag("chunk"));
                if (!chunk || chunk->value.size() < 2) continue;
                m_references.emplace(Game::DimensionToRaw(*dim), c->GetValue<std::string>("structure", ""),
                                     chunk->value[0], chunk->value[1]);
            }
        }
    }

    bool MapDataStore::SaveReferencesLocked() const {
        Game::Nbt::Writer w;
        w.BeginRootCompound();
        w.BeginCompound("data");
        std::map<int, std::vector<const ReferenceKey*>> byDim;
        for (const ReferenceKey& key : m_references) byDim[std::get<0>(key)].push_back(&key);
        for (const auto& [dim, keys] : byDim) {
            auto list = w.BeginList(Game::DimensionRegistryName(Game::DimensionFromRaw(dim)), Game::Nbt::TagType::Compound);
            for (const ReferenceKey* key : keys) {
                w.ListCompoundBegin(list);
                w.String("structure", std::get<1>(*key));
                const int32_t chunk[2] = {std::get<2>(*key), std::get<3>(*key)};
                w.IntArray("chunk", chunk, 2);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        w.EndCompound();
        w.Int("DataVersion", Game::Save::DataVersion());
        w.EndRootCompound();
        if (!w.ok()) return false;
        return WriteGzipFile(m_dataDir / kReferencesFile, w.Bytes());
    }

    bool MapDataStore::TryAddStructureReference(Game::DimensionId dim, const std::string& structure,
                                                int32_t chunkX, int32_t chunkZ) {
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadReferencesLocked();
        const bool added = m_references.emplace(Game::DimensionToRaw(dim), structure, chunkX, chunkZ).second;
        if (added) m_referencesDirty = true;
        return added;
    }

    // ── Saving ──────────────────────────────────────────────────────────

    void MapDataStore::SaveAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!Persistent()) return;
        int saved = 0;
        for (auto& [id, map] : m_maps) {
            if (!map->IsDirty()) continue;
            if (SaveMapLocked(id, *map)) {
                map->ClearDirty();
                ++saved;
            } else {
                Log::Error("[Maps] could not save map_%d.dat", id);
            }
        }
        if (m_indexDirty) {
            if (SaveIndexLocked()) m_indexDirty = false;
            else Log::Error("[Maps] could not save idcounts.dat");
        }
        if (m_referencesDirty) {
            if (SaveReferencesLocked()) m_referencesDirty = false;
            else Log::Error("[Maps] could not save %s", kReferencesFile);
        }
        if (saved > 0) Log::Info("[Maps] saved %d map(s)", saved);
    }

} // namespace Server
