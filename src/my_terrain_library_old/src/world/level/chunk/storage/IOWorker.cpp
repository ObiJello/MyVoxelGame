#include "world/level/chunk/storage/IOWorker.h"

// Reference: net/minecraft/world/level/chunk/storage/IOWorker.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

// Reference: IOWorker.java constructor lines 40-43
IOWorker::IOWorker(const RegionStorageInfo& info, const std::string& folder, bool sync)
    : m_storage(info, folder, sync)
{
    // Start worker thread
    m_workerThread = std::thread([this]() {
        workerLoop();
    });
}

IOWorker::~IOWorker() {
    close();
}

void IOWorker::workerLoop() {
    while (!m_shutdownRequested.load(std::memory_order_acquire)) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(m_mutex);

            // Wait for task or shutdown
            m_cv.wait(lock, [this]() {
                return !m_tasks.empty() || m_shutdownRequested.load(std::memory_order_acquire);
            });

            if (m_shutdownRequested.load(std::memory_order_acquire) && m_tasks.empty()) {
                break;
            }

            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
        }

        if (task) {
            try {
                task();
            } catch (...) {
                // Log error but continue processing
            }
        }
    }

    // Process any remaining pending writes before exiting
    processAllPendingWrites();
}

void IOWorker::submitTask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push(std::move(task));
    }
    m_cv.notify_one();
}

// Reference: IOWorker.java store(ChunkPos, CompoundTag)
std::shared_ptr<util::CompletableFuture<void>> IOWorker::store(
    const ChunkPos& pos,
    std::unique_ptr<nbt::CompoundTag> data)
{
    auto future = std::make_shared<util::CompletableFuture<void>>();
    int64_t key = pos.toLong();

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Check for existing pending write
        auto it = m_pendingWrites.find(key);
        if (it != m_pendingWrites.end()) {
            // Replace existing pending write
            it->second.data = std::move(data);
            it->second.supplier = nullptr;
            it->second.result = future;
        } else {
            // Create new pending write
            PendingStore pending;
            pending.data = std::move(data);
            pending.result = future;
            m_pendingWrites[key] = std::move(pending);
        }
    }

    // Submit task to process this write
    ChunkPos posCopy = pos;
    submitTask([this, posCopy]() {
        processPendingWrite(posCopy);
    });

    return future;
}

// Reference: IOWorker.java store(ChunkPos, Supplier<CompoundTag>)
std::shared_ptr<util::CompletableFuture<void>> IOWorker::store(
    const ChunkPos& pos,
    std::function<std::unique_ptr<nbt::CompoundTag>()> supplier)
{
    auto future = std::make_shared<util::CompletableFuture<void>>();
    int64_t key = pos.toLong();

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Check for existing pending write
        auto it = m_pendingWrites.find(key);
        if (it != m_pendingWrites.end()) {
            // Replace existing pending write
            it->second.data = nullptr;
            it->second.supplier = std::move(supplier);
            it->second.result = future;
        } else {
            // Create new pending write
            PendingStore pending;
            pending.supplier = std::move(supplier);
            pending.result = future;
            m_pendingWrites[key] = std::move(pending);
        }
    }

    // Submit task to process this write
    ChunkPos posCopy = pos;
    submitTask([this, posCopy]() {
        processPendingWrite(posCopy);
    });

    return future;
}

void IOWorker::processPendingWrite(const ChunkPos& pos) {
    int64_t key = pos.toLong();
    std::unique_ptr<nbt::CompoundTag> data;
    std::shared_ptr<util::CompletableFuture<void>> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_pendingWrites.find(key);
        if (it == m_pendingWrites.end()) {
            return;
        }

        // Get data from pending write
        if (it->second.data) {
            data = std::move(it->second.data);
        } else if (it->second.supplier) {
            data = it->second.supplier();
        }
        future = it->second.result;
        m_pendingWrites.erase(it);
    }

    // Write to storage (outside lock)
    try {
        m_storage.write(pos, data.get());
        if (future) {
            future->complete();
        }
    } catch (...) {
        if (future) {
            future->completeExceptionally(std::current_exception());
        }
    }
}

void IOWorker::processAllPendingWrites() {
    std::map<int64_t, PendingStore> pending;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending = std::move(m_pendingWrites);
        m_pendingWrites.clear();
    }

    for (auto& pair : pending) {
        ChunkPos pos = ChunkPos::fromLong(pair.first);

        std::unique_ptr<nbt::CompoundTag> data;
        if (pair.second.data) {
            data = std::move(pair.second.data);
        } else if (pair.second.supplier) {
            data = pair.second.supplier();
        }

        try {
            m_storage.write(pos, data.get());
            if (pair.second.result) {
                pair.second.result->complete();
            }
        } catch (...) {
            if (pair.second.result) {
                pair.second.result->completeExceptionally(std::current_exception());
            }
        }
    }
}

// Reference: IOWorker.java loadAsync(ChunkPos)
std::shared_ptr<util::CompletableFuture<std::optional<std::unique_ptr<nbt::CompoundTag>>>>
IOWorker::loadAsync(const ChunkPos& pos)
{
    auto future = std::make_shared<util::CompletableFuture<std::optional<std::unique_ptr<nbt::CompoundTag>>>>();
    int64_t key = pos.toLong();

    // Check for pending write first
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pendingWrites.find(key);
        if (it != m_pendingWrites.end()) {
            // Return a copy of pending data
            if (it->second.data) {
                auto copy = std::unique_ptr<nbt::CompoundTag>(
                    static_cast<nbt::CompoundTag*>(it->second.data->copy().release()));
                future->complete(std::optional<std::unique_ptr<nbt::CompoundTag>>(std::move(copy)));
                return future;
            } else if (it->second.supplier) {
                // Generate data from supplier
                auto data = it->second.supplier();
                future->complete(std::optional<std::unique_ptr<nbt::CompoundTag>>(std::move(data)));
                return future;
            }
        }
    }

    // Submit async load task
    ChunkPos posCopy = pos;
    submitTask([this, posCopy, future]() {
        try {
            auto data = m_storage.read(posCopy);
            if (data) {
                future->complete(std::optional<std::unique_ptr<nbt::CompoundTag>>(std::move(data)));
            } else {
                future->complete(std::nullopt);
            }
        } catch (...) {
            future->completeExceptionally(std::current_exception());
        }
    });

    return future;
}

std::unique_ptr<nbt::CompoundTag> IOWorker::load(const ChunkPos& pos) {
    int64_t key = pos.toLong();

    // Check for pending write first
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pendingWrites.find(key);
        if (it != m_pendingWrites.end()) {
            // Return a copy of pending data
            if (it->second.data) {
                return std::unique_ptr<nbt::CompoundTag>(
                    static_cast<nbt::CompoundTag*>(it->second.data->copy().release()));
            } else if (it->second.supplier) {
                return it->second.supplier();
            }
        }
    }

    // Read directly from storage
    return m_storage.read(pos);
}

bool IOWorker::hasChunk(const ChunkPos& pos) {
    int64_t key = pos.toLong();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pendingWrites.find(key) != m_pendingWrites.end()) {
            return true;
        }
    }

    return m_storage.hasChunk(pos);
}

// Reference: IOWorker.java synchronize(boolean)
std::shared_ptr<util::CompletableFuture<void>> IOWorker::synchronize(bool flush) {
    auto future = std::make_shared<util::CompletableFuture<void>>();

    submitTask([this, flush, future]() {
        processAllPendingWrites();
        if (flush) {
            m_storage.flush();
        }
        future->complete();
    });

    return future;
}

// Reference: IOWorker.java close()
void IOWorker::close() {
    if (m_shutdownRequested.exchange(true, std::memory_order_acq_rel)) {
        return;  // Already shutting down
    }

    m_cv.notify_all();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_storage.close();
}

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
