// File: src/server/world/storage/anvil/RegionStore.cpp
#include "server/world/storage/anvil/RegionStore.hpp"

#include "common/core/Log.hpp"


namespace Game::Anvil {

    RegionStore::RegionStore(SaveRoot root)
        : m_worldRoot(root.Root())
        , m_writable(true) {}

    std::unique_ptr<RegionStore> RegionStore::OpenReadOnly(std::filesystem::path worldRoot) {
        // The private (path, writable) constructor, so this is the only way to
        // get a store for a folder that has no SaveRoot — and it is read-only.
        return std::unique_ptr<RegionStore>(new RegionStore(std::move(worldRoot), false));
    }

    RegionStore::~RegionStore() { CloseAll(); }

    std::filesystem::path RegionStore::FilePath(DimensionId dim, RegionKind kind, int rx, int rz) const {
        // MC's layout: the overworld at the world root, the others in DIM-1 /
        // DIM1 beside it.
        const std::string_view subdir = DimensionSaveSubdir(dim);
        std::filesystem::path dir = subdir.empty() ? m_worldRoot
                                                   : m_worldRoot / std::filesystem::path(subdir);
        dir /= (kind == RegionKind::Entities) ? "entities" : "region";
        return dir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
    }

    AnvilRegion* RegionStore::Get(DimensionId dim, RegionKind kind, int rx, int rz,
                                  std::string& error, bool createIfMissing) {
        error.clear();

        const Key key{dim, kind, static_cast<int32_t>(rx), static_cast<int32_t>(rz)};

        if (auto it = m_open.find(key); it != m_open.end()) {
            m_lru.splice(m_lru.begin(), m_lru, it->second.lruPos);   // touch
            return it->second.region.get();
        }

        const auto path = FilePath(dim, kind, rx, rz);

        // Asking about a chunk must never create the file. Only a write may.
        if (!createIfMissing) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) return nullptr;
        }

        auto region = AnvilRegion::Open(path, m_writable, error);
        if (!region) {
            if (!error.empty()) {
                Log::Warning("[Anvil] cannot open %s: %s", path.filename().string().c_str(), error.c_str());
            }
            return nullptr;
        }

        // Evict before inserting so the cache never exceeds its bound. The
        // evicted region flushes and closes in its destructor.
        while (m_lru.size() >= kMaxOpenRegions) {
            const Key victim = m_lru.back();
            m_lru.pop_back();
            m_open.erase(victim);
        }

        m_lru.push_front(key);
        AnvilRegion* raw = region.get();
        m_open.emplace(key, Entry{std::move(region), m_lru.begin()});
        return raw;
    }

    void RegionStore::CloseAll() {
        if (m_open.empty()) return;
        m_open.clear();     // ~AnvilRegion flushes and pads each writable file
        m_lru.clear();
    }

} // namespace Game::Anvil
