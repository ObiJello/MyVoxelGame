// File: src/world/RegionFileCache.cpp
#include "RegionFileCache.hpp"
#include "../core/Log.hpp"
#include <filesystem>
#include <sstream>

namespace World {

    RegionFileCache& RegionFileCache::Instance() {
        static RegionFileCache instance;
        return instance;
    }

    std::shared_ptr<RegionFile> RegionFileCache::GetRegionFile(int regionX, int regionZ, const std::string& worldPath) {
        std::string filePath = GenerateRegionFilePath(regionX, regionZ, worldPath);
        return GetRegionFile(filePath);
    }

    std::shared_ptr<RegionFile> RegionFileCache::GetRegionFile(const std::string& filePath) {
        std::lock_guard<std::mutex> lock(cacheMutex);

        // Check if already cached
        auto it = cache.find(filePath);
        if (it != cache.end()) {
            Log::Debug("Region file cache hit: %s", filePath.c_str());
            return it->second;
        }

        // Not in cache, try to load
        Log::Debug("Region file cache miss, loading: %s", filePath.c_str());
        return LoadAndCache(filePath);
    }

    void RegionFileCache::Clear() {
        std::lock_guard<std::mutex> lock(cacheMutex);

        size_t cacheSize = cache.size();
        cache.clear();

        Log::Info("Region file cache cleared (%zu entries)", cacheSize);
    }

    size_t RegionFileCache::GetCacheSize() const {
        std::lock_guard<std::mutex> lock(cacheMutex);
        return cache.size();
    }

    std::string RegionFileCache::GenerateRegionFilePath(int regionX, int regionZ, const std::string& worldPath) {
        // Minecraft region file naming: r.<regionX>.<regionZ>.mca
        std::ostringstream oss;
        oss << worldPath;

        // Add separator if worldPath doesn't end with one
        if (!worldPath.empty() && worldPath.back() != '/' && worldPath.back() != '\\') {
            oss << "/";
        }

        oss << "region/r." << regionX << "." << regionZ << ".mca";
        return oss.str();
    }

    std::shared_ptr<RegionFile> RegionFileCache::LoadAndCache(const std::string& filePath) {
        // Check if file exists
        if (!std::filesystem::exists(filePath)) {
            Log::Debug("Region file does not exist: %s", filePath.c_str());
            // Cache nullptr to avoid repeated filesystem checks
            cache[filePath] = nullptr;
            return nullptr;
        }

        // Create and load region file
        auto regionFile = std::make_shared<RegionFile>(filePath);

        if (!regionFile->LoadHeader()) {
            Log::Warning("Failed to load region file header: %s", filePath.c_str());
            // Cache nullptr to indicate load failure
            cache[filePath] = nullptr;
            return nullptr;
        }

        // Successfully loaded, add to cache
        cache[filePath] = regionFile;
        Log::Info("Region file loaded and cached: %s", filePath.c_str());

        return regionFile;
    }

} // namespace World