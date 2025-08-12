// File: src/server/world/storage/RegionFileCache.hpp
#pragma once

#include "RegionFile.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>

namespace World {

    // Cache for region files to avoid repeatedly loading headers
    class RegionFileCache {
    public:
        // Get region file for given region coordinates
        // Returns nullptr if file doesn't exist or can't be loaded
        std::shared_ptr<RegionFile> GetRegionFile(int regionX, int regionZ, const std::string& worldPath);

        // Get region file by direct path
        std::shared_ptr<RegionFile> GetRegionFile(const std::string& filePath);

        // Clear all cached region files
        void Clear();

        // Get cache statistics
        size_t GetCacheSize() const;

        // Get singleton instance
        static RegionFileCache& Instance();

    private:
        RegionFileCache() = default;

        mutable std::mutex cacheMutex;
        std::unordered_map<std::string, std::shared_ptr<RegionFile>> cache;

        // Generate region file path from coordinates and world path
        static std::string GenerateRegionFilePath(int regionX, int regionZ, const std::string& worldPath);

        // Load region file and add to cache
        std::shared_ptr<RegionFile> LoadAndCache(const std::string& filePath);
    };

} // namespace World