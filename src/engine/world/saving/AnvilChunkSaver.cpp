#include "AnvilChunkSaver.hpp"
#include "../Chunk.hpp"
#include "../../../core/JobSystem.hpp"
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace Game {

    AnvilChunkSaver::AnvilChunkSaver(const AnvilSaverConfig& config)
        : m_config(config) {
        
        Log::Debug("AnvilChunkSaver created for world: %s", config.worldPath.c_str());
    }

    AnvilChunkSaver::~AnvilChunkSaver() {
        Shutdown();
    }

    // === CORE SAVING INTERFACE ===

    ChunkSaveResult AnvilChunkSaver::SaveChunk(const Chunk& chunk) {
        if (!m_initialized) {
            return ChunkSaveResult::Failure(chunk.pos, "Saver not initialized");
        }

        auto startTime = std::chrono::high_resolution_clock::now();
        ChunkSaveResult result = SaveChunkInternal(chunk);
        auto endTime = std::chrono::high_resolution_clock::now();
        
        result.saveTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        UpdateStats(result);
        
        return result;
    }

    std::future<ChunkSaveResult> AnvilChunkSaver::SaveChunkAsync(const Chunk& chunk) {
        if (!m_initialized) {
            std::promise<ChunkSaveResult> promise;
            promise.set_value(ChunkSaveResult::Failure(chunk.pos, "Saver not initialized"));
            return promise.get_future();
        }

        auto promise = std::make_shared<std::promise<ChunkSaveResult>>();
        auto future = promise->get_future();

        // Submit to job system
        JobSystem::g_ThreadPool.Enqueue([this, chunk, promise]() {
            ChunkSaveResult result = SaveChunk(chunk);
            promise->set_value(std::move(result));
        });

        return future;
    }

    void AnvilChunkSaver::QueueChunkForSave(std::shared_ptr<const Chunk> chunk) {
        if (!chunk || !m_initialized) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_queueMutex);
        
        // Check if already queued
        if (m_pendingChunks.find(chunk->pos) != m_pendingChunks.end()) {
            return;
        }

        m_saveQueue.push(chunk);
        m_pendingChunks.insert(chunk->pos);
    }

    std::vector<ChunkSaveResult> AnvilChunkSaver::FlushPendingSaves() {
        std::vector<ChunkSaveResult> results;

        if (!m_initialized) {
            return results;
        }

        // Process all queued chunks
        ProcessSaveQueue();

        return results;
    }

    std::future<std::vector<ChunkSaveResult>> AnvilChunkSaver::FlushPendingSavesAsync() {
        auto promise = std::make_shared<std::promise<std::vector<ChunkSaveResult>>>();
        auto future = promise->get_future();

        JobSystem::g_ThreadPool.Enqueue([this, promise]() {
            std::vector<ChunkSaveResult> results = FlushPendingSaves();
            promise->set_value(std::move(results));
        });

        return future;
    }

    // === BATCH OPERATIONS ===

    std::vector<ChunkSaveResult> AnvilChunkSaver::SaveChunks(const std::vector<std::shared_ptr<const Chunk>>& chunks) {
        std::vector<ChunkSaveResult> results;
        results.reserve(chunks.size());

        for (const auto& chunk : chunks) {
            if (chunk) {
                results.push_back(SaveChunk(*chunk));
            } else {
                results.push_back(ChunkSaveResult::Failure(Math::ChunkPos{0, 0}, "Null chunk"));
            }
        }

        return results;
    }

    // === CONFIGURATION ===

    void AnvilChunkSaver::SetDestination(const std::string& destinationPath) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.worldPath = destinationPath;
    }

    std::string AnvilChunkSaver::GetDestination() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.worldPath;
    }

    void AnvilChunkSaver::SetCompressionEnabled(bool enabled) {
        // Anvil format always uses compression, so this is ignored
        Log::Debug("AnvilChunkSaver: Compression is always enabled for Anvil format");
    }

    bool AnvilChunkSaver::IsCompressionEnabled() const {
        return true; // Always true for Anvil format
    }

    void AnvilChunkSaver::SetCompressionLevel(int level) {
        // Anvil uses Zlib default compression, level setting ignored
        Log::Debug("AnvilChunkSaver: Compression level is fixed for Anvil format");
    }

    int AnvilChunkSaver::GetCompressionLevel() const {
        return 6; // Zlib default
    }

    // === SAVE POLICIES ===

    void AnvilChunkSaver::SetSaveMode(IChunkSaver::SaveMode mode) {  // FIX: Use fully qualified name
        // Anvil saver uses immediate mode by default
    }

    IChunkSaver::SaveMode AnvilChunkSaver::GetSaveMode() const {  // FIX: Use fully qualified name
        return IChunkSaver::SaveMode::Immediate;
    }

    void AnvilChunkSaver::SetAutoSaveEnabled(bool enabled) {
        // Auto-save would be handled by the calling system
    }

    void AnvilChunkSaver::SetAutoSaveInterval(float seconds) {
        // Auto-save interval handled by calling system
    }

    bool AnvilChunkSaver::IsAutoSaveEnabled() const {
        return false; // Handled externally
    }

    float AnvilChunkSaver::GetAutoSaveInterval() const {
        return 0.0f; // Handled externally
    }

    // === LIFECYCLE ===

    bool AnvilChunkSaver::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Info("Initializing AnvilChunkSaver...");

        if (m_config.worldPath.empty()) {
            SetLastError("No world path specified");
            return false;
        }

        // Create world structure if needed
        if (m_config.createWorldStructure) {
            if (!CreateWorldStructure(m_config.worldPath)) {
                SetLastError("Failed to create world structure");
                return false;
            }
        }

        // Validate world path
        if (!ValidateWorldPath(m_config.worldPath)) {
            SetLastError("Invalid world path: " + m_config.worldPath);
            return false;
        }

        // Reset statistics
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.Reset();
        }

        m_initialized = true;
        Log::Info("AnvilChunkSaver initialized for world: %s", m_config.worldPath.c_str());
        return true;
    }

    void AnvilChunkSaver::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down AnvilChunkSaver...");

        m_shutdownRequested = true;

        // Flush any pending saves
        FlushPendingSaves();

        // Close all region files
        FlushRegionCache();

        // Clear queues
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_saveQueue.empty()) {
                m_saveQueue.pop();
            }
            m_pendingChunks.clear();
        }

        m_initialized = false;
        Log::Info("AnvilChunkSaver shutdown complete");
    }

    bool AnvilChunkSaver::IsReady() const {
        return m_initialized && !m_shutdownRequested;
    }

    // === QUEUE MANAGEMENT ===

    size_t AnvilChunkSaver::GetPendingChunkCount() const {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_saveQueue.size();
    }

    void AnvilChunkSaver::ClearPendingChunks() {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_saveQueue.empty()) {
            m_saveQueue.pop();
        }
        m_pendingChunks.clear();
    }

    bool AnvilChunkSaver::IsChunkPending(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_pendingChunks.find(position) != m_pendingChunks.end();
    }

    bool AnvilChunkSaver::RemovePendingChunk(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_pendingChunks.erase(position) > 0;
    }

    // === STATISTICS ===

    AnvilChunkSaver::SaverStats AnvilChunkSaver::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    void AnvilChunkSaver::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.Reset();
    }

    // === VALIDATION ===

    bool AnvilChunkSaver::CanSaveChunk(const Chunk& chunk) const {
        return ValidateChunkForAnvil(chunk);
    }

    bool AnvilChunkSaver::VerifySavedChunk(Math::ChunkPos position) {
        int regionX, regionZ;
        GetRegionCoords(position, regionX, regionZ);

        std::string regionPath = GetRegionFilePath(regionX, regionZ);
        return std::filesystem::exists(regionPath) && ValidateAnvilFormat(regionPath);
    }

    // === ERROR HANDLING ===

    std::string AnvilChunkSaver::GetLastError() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    void AnvilChunkSaver::ClearErrors() {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        const_cast<std::string&>(m_lastError).clear();  // FIX: Use const_cast for mutable member
    }

    void AnvilChunkSaver::SetErrorPolicy(IChunkSaver::ErrorPolicy policy) {  // FIX: Use fully qualified name
        m_errorPolicy = policy;
    }

    AnvilChunkSaver::ErrorPolicy AnvilChunkSaver::GetErrorPolicy() const {
        return m_errorPolicy;
    }

    // === BACKUP AND RECOVERY ===

    void AnvilChunkSaver::SetBackupEnabled(bool enabled) {
        // Backup functionality could be implemented here
        Log::Debug("AnvilChunkSaver: Backup functionality not yet implemented");
    }

    bool AnvilChunkSaver::IsBackupEnabled() const {
        return false; // Not implemented yet
    }

    void AnvilChunkSaver::SetMaxBackups(int maxBackups) {
        // Not implemented yet
    }

    int AnvilChunkSaver::GetMaxBackups() const {
        return 0; // Not implemented yet
    }

    // === ANVIL-SPECIFIC FEATURES ===

    void AnvilChunkSaver::SetRegionCacheSize(size_t maxRegions) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.maxCachedRegions = maxRegions;

        // Clean up excess regions if needed
        CleanupRegionCache();
    }

    size_t AnvilChunkSaver::GetRegionCacheSize() const {
        std::lock_guard<std::mutex> lock(m_regionCacheMutex);
        return m_regionCache.size();
    }

    void AnvilChunkSaver::FlushRegionCache() {
        std::lock_guard<std::mutex> lock(m_regionCacheMutex);

        // Finalize all region writers
        for (auto& [key, writer] : m_regionCache) {
            if (writer) {
                writer->Finalize();
            }
        }

        m_regionCache.clear();
        m_regionAccessTimes.clear();

        Log::Debug("AnvilChunkSaver: Flushed region cache");
    }

    void AnvilChunkSaver::CloseRegionFile(int regionX, int regionZ) {
        uint64_t key = GetRegionKey(regionX, regionZ);
        CloseRegion(key);
    }

    std::vector<std::pair<int, int>> AnvilChunkSaver::GetCachedRegions() const {
        std::lock_guard<std::mutex> lock(m_regionCacheMutex);

        std::vector<std::pair<int, int>> regions;
        for (const auto& [key, writer] : m_regionCache) {
            int regionX = static_cast<int>(key >> 32);
            int regionZ = static_cast<int>(key & 0xFFFFFFFF);
            regions.emplace_back(regionX, regionZ);
        }

        return regions;
    }

    bool AnvilChunkSaver::ValidateAnvilFormat(const std::string& regionFilePath) const {
        if (!std::filesystem::exists(regionFilePath)) {
            return false;
        }

        // Basic validation: check file size and header
        std::ifstream file(regionFilePath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        // Check minimum file size (8KB header)
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();

        return fileSize >= AnvilRegionWriter::HEADER_SIZE;
    }

    bool AnvilChunkSaver::CreateWorldStructure(const std::string& worldPath) const {
        return Game::CreateMinecraftWorldStructure(worldPath);
    }

    void AnvilChunkSaver::SetStrictCompliance(bool enabled) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.strictAnvilCompliance = enabled;
    }

    bool AnvilChunkSaver::IsStrictCompliance() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.strictAnvilCompliance;
    }

    // === PROTECTED METHODS ===

    void AnvilChunkSaver::UpdateStats(const ChunkSaveResult& result) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (result.success) {
            m_stats.chunksSaved++;
            m_stats.totalSaveTimeMs += result.saveTimeMs;
            m_stats.totalBytesWritten += result.bytesWritten;

            if (m_stats.chunksSaved > 0) {
                m_stats.averageSaveTimeMs = m_stats.totalSaveTimeMs / m_stats.chunksSaved;
            }
        } else {
            m_stats.saveFailures++;
        }
    }

    // === PRIVATE IMPLEMENTATION ===

    std::shared_ptr<AnvilRegionWriter> AnvilChunkSaver::GetRegionWriter(Math::ChunkPos chunkPos) {
        int regionX, regionZ;
        GetRegionCoords(chunkPos, regionX, regionZ);

        uint64_t key = GetRegionKey(regionX, regionZ);

        std::lock_guard<std::mutex> lock(m_regionCacheMutex);

        // Check if already cached
        auto it = m_regionCache.find(key);
        if (it != m_regionCache.end()) {
            m_regionAccessTimes[key] = std::chrono::steady_clock::now();
            return it->second;
        }

        // Create new region writer
        auto writer = CreateRegionWriter(regionX, regionZ);
        if (writer) {
            m_regionCache[key] = writer;
            m_regionAccessTimes[key] = std::chrono::steady_clock::now();

            // Clean up old regions if cache is full
            if (m_regionCache.size() > m_config.maxCachedRegions) {
                CleanupRegionCache();
            }
        }

        return writer;
    }

    std::shared_ptr<AnvilRegionWriter> AnvilChunkSaver::CreateRegionWriter(int regionX, int regionZ) {
        std::string regionPath = GetRegionFilePath(regionX, regionZ);

        auto writer = std::make_shared<AnvilRegionWriter>(regionPath);
        if (!writer->Initialize()) {
            LogError("CreateRegionWriter", "Failed to initialize region writer for " + regionPath);
            return nullptr;
        }

        Log::Debug("Created region writer for (%d, %d): %s", regionX, regionZ, regionPath.c_str());
        return writer;
    }

    uint64_t AnvilChunkSaver::GetRegionKey(int regionX, int regionZ) const {
        return (static_cast<uint64_t>(regionX) << 32) | static_cast<uint64_t>(regionZ);
    }

    void AnvilChunkSaver::CleanupRegionCache() {
        auto now = std::chrono::steady_clock::now();

        // Remove expired regions
        auto it = m_regionAccessTimes.begin();
        while (it != m_regionAccessTimes.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second);

            if (elapsed.count() > m_config.regionCacheTimeoutSeconds) {
                uint64_t key = it->first;
                CloseRegion(key);
                it = m_regionAccessTimes.erase(it);
            } else {
                ++it;
            }
        }

        // Remove excess regions if still over limit
        while (m_regionCache.size() > m_config.maxCachedRegions && !m_regionCache.empty()) {
            // Find oldest accessed region
            auto oldest = std::min_element(m_regionAccessTimes.begin(), m_regionAccessTimes.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });

            if (oldest != m_regionAccessTimes.end()) {
                CloseRegion(oldest->first);
                m_regionAccessTimes.erase(oldest);
            } else {
                break;
            }
        }
    }

    void AnvilChunkSaver::CloseRegion(uint64_t regionKey) {
        auto it = m_regionCache.find(regionKey);
        if (it != m_regionCache.end()) {
            if (it->second) {
                it->second->Finalize();
            }
            m_regionCache.erase(it);
        }
    }

    ChunkSaveResult AnvilChunkSaver::SaveChunkInternal(const Chunk& chunk) {
        if (!ValidateChunkForAnvil(chunk)) {
            return ChunkSaveResult::Failure(chunk.pos, "Chunk validation failed");
        }

        try {
            // Get region writer
            auto writer = GetRegionWriter(chunk.pos);
            if (!writer) {
                return ChunkSaveResult::Failure(chunk.pos, "Failed to get region writer");
            }

            // Calculate local coordinates
            int regionX, regionZ;
            GetRegionCoords(chunk.pos, regionX, regionZ);

            int localX = chunk.pos.x - (regionX * 32);
            int localZ = chunk.pos.z - (regionZ * 32);

            // Write chunk to region
            if (!writer->WriteChunk(localX, localZ, chunk)) {
                return ChunkSaveResult::Failure(chunk.pos, "Failed to write chunk to region");
            }

            // Estimate bytes written (simplified)
            size_t bytesWritten = chunk.GetNonAirBlockCount() * 2; // Rough estimate

            ChunkSaveResult result = ChunkSaveResult::Success(chunk.pos, bytesWritten);
            result.wasCompressed = true; // Anvil always uses compression

            return result;

        } catch (const std::exception& e) {
            std::string error = "Exception saving chunk: " + std::string(e.what());
            SetLastError(error);
            return ChunkSaveResult::Failure(chunk.pos, error);
        }
    }

    void AnvilChunkSaver::ProcessSaveQueue() {
        std::queue<std::shared_ptr<const Chunk>> chunksToSave;

        // Move chunks from queue to local processing queue
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            chunksToSave = std::move(m_saveQueue);
            m_saveQueue = std::queue<std::shared_ptr<const Chunk>>();
            m_pendingChunks.clear();
        }

        // Process chunks
        while (!chunksToSave.empty()) {
            auto chunk = chunksToSave.front();
            chunksToSave.pop();

            if (chunk) {
                SaveChunk(*chunk);
            }
        }
    }

    bool AnvilChunkSaver::ValidateChunkForAnvil(const Chunk& chunk) const {
        // Basic validation
        if (chunk.IsEmpty()) {
            return false; // Don't save empty chunks
        }

        // Validate chunk position is reasonable
        const int MAX_COORD = 30000000; // Minecraft world limits
        if (std::abs(chunk.pos.x) > MAX_COORD || std::abs(chunk.pos.z) > MAX_COORD) {
            return false;
        }

        return true;
    }

    bool AnvilChunkSaver::ValidateWorldPath(const std::string& worldPath) const {
        if (worldPath.empty()) {
            return false;
        }

        try {
            std::filesystem::path path(worldPath);

            // Check if directory exists or can be created
            if (!std::filesystem::exists(path)) {
                if (!m_config.createWorldStructure) {
                    return false;
                }
            } else if (!std::filesystem::is_directory(path)) {
                return false;
            }

            // Check region directory
            std::filesystem::path regionPath = path / "region";
            if (std::filesystem::exists(regionPath) && !std::filesystem::is_directory(regionPath)) {
                return false;
            }

            return true;

        } catch (const std::exception& e) {
            LogError("ValidateWorldPath", e.what());
            return false;
        }
    }

    void AnvilChunkSaver::SetLastError(const std::string& error) const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        const_cast<std::string&>(m_lastError) = error;  // FIX: Use const_cast for mutable member
    }

    void AnvilChunkSaver::LogError(const std::string& operation, const std::string& error) const {
        Log::Error("AnvilChunkSaver %s: %s", operation.c_str(), error.c_str());
        SetLastError(operation + ": " + error);
    }

    void AnvilChunkSaver::GetRegionCoords(Math::ChunkPos chunkPos, int& regionX, int& regionZ) const {
        AnvilRegionWriter::ChunkToRegion(chunkPos, regionX, regionZ, regionX, regionZ);
    }

    bool AnvilChunkSaver::RegionFileExists(int regionX, int regionZ) const {
        std::string regionPath = GetRegionFilePath(regionX, regionZ);
        return std::filesystem::exists(regionPath);
    }

    std::string AnvilChunkSaver::GetRegionFilePath(int regionX, int regionZ) const {
        return AnvilRegionWriter::GenerateRegionFilePath(m_config.worldPath, regionX, regionZ);
    }

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<IChunkSaver> CreateAnvilChunkSaver(const AnvilSaverConfig& config) {
        return std::make_unique<AnvilChunkSaver>(config);
    }

    bool ConvertToAnvilFormat(const std::string& inputPath, const std::string& outputPath) {
        // Conversion functionality would be implemented here
        Log::Warning("ConvertToAnvilFormat: Not yet implemented");
        return false;
    }

    AnvilValidationResult ValidateAnvilWorld(const std::string& worldPath) {
        AnvilValidationResult result;
        
        try {
            std::filesystem::path path(worldPath);
            
            if (!std::filesystem::exists(path)) {
                result.errors.push_back("World path does not exist: " + worldPath);
                return result;
            }
            
            std::filesystem::path regionPath = path / "region";
            if (!std::filesystem::exists(regionPath)) {
                result.errors.push_back("Region directory does not exist");
                return result;
            }
            
            // Scan region files
            for (const auto& entry : std::filesystem::directory_iterator(regionPath)) {
                if (entry.path().extension() == ".mca") {
                    result.totalRegions++;
                    
                    // Basic validation of region file
                    std::ifstream file(entry.path(), std::ios::binary);
                    if (file.is_open()) {
                        file.seekg(0, std::ios::end);
                        size_t fileSize = file.tellg();
                        
                        if (fileSize >= AnvilRegionWriter::HEADER_SIZE) {
                            result.validRegions++;
                        } else {
                            result.warnings.push_back("Region file too small: " + entry.path().filename().string());
                        }
                    } else {
                        result.errors.push_back("Cannot read region file: " + entry.path().filename().string());
                    }
                }
            }
            
            result.isValid = result.errors.empty() && result.validRegions > 0;
            
        } catch (const std::exception& e) {
            result.errors.push_back("Exception during validation: " + std::string(e.what()));
        }
        
        return result;
    }

} // namespace Game