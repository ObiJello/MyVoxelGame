#pragma once

#include "world/level/chunk/storage/RegionFileStorage.h"
#include "world/ChunkPos.h"
#include "nbt/CompoundTag.h"
#include "util/CompletableFuture.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>

// Reference: net/minecraft/world/level/chunk/storage/IOWorker.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

/**
 * ChunkScanAccess - Interface for scanning chunks
 * Reference: ChunkScanAccess.java
 */
class ChunkScanAccess {
public:
    virtual ~ChunkScanAccess() = default;
};

/**
 * IOWorker - Async I/O worker for chunk data
 * Reference: IOWorker.java
 *
 * Provides async loading and storing of chunk NBT data with a pending writes queue.
 * Uses a background thread to process I/O operations.
 */
class IOWorker : public ChunkScanAccess {
public:
    // Reference: IOWorker.java constructor lines 40-43
    IOWorker(const RegionStorageInfo& info, const std::string& folder, bool sync);

    ~IOWorker();

    // =========================================================================
    // Async load/store operations
    // Reference: IOWorker.java lines 122-150
    // =========================================================================

    /**
     * Store chunk data asynchronously
     * Reference: IOWorker.java store(ChunkPos, CompoundTag)
     */
    std::shared_ptr<util::CompletableFuture<void>> store(
        const ChunkPos& pos,
        std::unique_ptr<nbt::CompoundTag> data);

    /**
     * Store chunk data asynchronously using supplier
     * Reference: IOWorker.java store(ChunkPos, Supplier<CompoundTag>)
     */
    std::shared_ptr<util::CompletableFuture<void>> store(
        const ChunkPos& pos,
        std::function<std::unique_ptr<nbt::CompoundTag>()> supplier);

    /**
     * Load chunk data asynchronously
     * Reference: IOWorker.java loadAsync(ChunkPos)
     */
    std::shared_ptr<util::CompletableFuture<std::optional<std::unique_ptr<nbt::CompoundTag>>>>
    loadAsync(const ChunkPos& pos);

    /**
     * Load chunk data synchronously (blocks until complete)
     */
    std::unique_ptr<nbt::CompoundTag> load(const ChunkPos& pos);

    // =========================================================================
    // Synchronization
    // Reference: IOWorker.java lines 152-163
    // =========================================================================

    /**
     * Synchronize - wait for pending writes to complete
     * Reference: IOWorker.java synchronize(boolean)
     */
    std::shared_ptr<util::CompletableFuture<void>> synchronize(bool flush);

    /**
     * Check if chunk has pending write or exists on disk
     */
    bool hasChunk(const ChunkPos& pos);

    // =========================================================================
    // Lifecycle
    // Reference: IOWorker.java lines 232-244
    // =========================================================================

    /**
     * Close the worker and wait for pending operations
     * Reference: IOWorker.java close()
     */
    void close();

    /**
     * Check if shutdown has been requested
     */
    bool isShutdownRequested() const {
        return m_shutdownRequested.load(std::memory_order_acquire);
    }

private:
    struct PendingStore {
        std::unique_ptr<nbt::CompoundTag> data;
        std::function<std::unique_ptr<nbt::CompoundTag>()> supplier;
        std::shared_ptr<util::CompletableFuture<void>> result;
    };

    struct LoadRequest {
        ChunkPos pos;
        std::shared_ptr<util::CompletableFuture<std::optional<std::unique_ptr<nbt::CompoundTag>>>> result;
    };

    RegionFileStorage m_storage;
    std::map<int64_t, PendingStore> m_pendingWrites;  // Keyed by ChunkPos.toLong()

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_shutdownRequested{false};

    std::thread m_workerThread;
    std::queue<std::function<void()>> m_tasks;

    void workerLoop();
    void submitTask(std::function<void()> task);
    void processPendingWrite(const ChunkPos& pos);
    void processAllPendingWrites();
};

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
