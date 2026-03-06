#include "server/level/GenerationChunkHolder.h"
#include "server/level/ChunkLevel.h"
#include "server/level/ChunkMap.h"
#include "server/level/ChunkGenerationTask.h"
#include <stdexcept>
#include <thread>
#include <iostream>

// Reference: net/minecraft/server/level/GenerationChunkHolder.java

namespace minecraft {
namespace server {
namespace level {

// Static member definitions
GenerationChunkHolder::ChunkResultType GenerationChunkHolder::NOT_DONE_YET;
GenerationChunkHolder::ChunkResultType GenerationChunkHolder::UNLOADED_CHUNK;
GenerationChunkHolder::FutureType GenerationChunkHolder::UNLOADED_CHUNK_FUTURE;

void GenerationChunkHolder::initializeStatics() {
    // Reference: GenerationChunkHolder.java lines 26-28
    NOT_DONE_YET = ChunkResult<ChunkAccess*>::error("Not done yet");
    UNLOADED_CHUNK = ChunkResult<ChunkAccess*>::error("Unloaded chunk");
    UNLOADED_CHUNK_FUTURE = util::CompletableFuture<ChunkResultType>::completedFuture(UNLOADED_CHUNK);
}

GenerationChunkHolder::GenerationChunkHolder(const world::ChunkPos& pos)
    : m_pos(pos)
    , m_generationSaveSyncFuture(util::CompletableFuture<void>::completedFuture())
{
    // Reference: GenerationChunkHolder.java lines 37-46
    // Initialize futures array to nullptr (already default initialized)

    if (!pos.isValid()) {
        throw std::invalid_argument("Trying to create chunk out of reasonable bounds: " + pos.toString());
    }
}

GenerationChunkHolder::FutureType GenerationChunkHolder::scheduleChunkGenerationTask(
    const world::chunk::status::ChunkStatus& status,
    ChunkMap& scheduler
) {
    // Reference: GenerationChunkHolder.java lines 48-64
    if (isStatusDisallowed(status)) {
        return UNLOADED_CHUNK_FUTURE;
    }

    FutureType future = getOrCreateFuture(status);
    if (future->isDone()) {
        return future;
    }

    std::shared_ptr<ChunkGenerationTask> task;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        task = m_task;
    }
    bool needsReschedule = false;
    if (task == nullptr) {
        needsReschedule = true;
    } else {
        const world::chunk::status::ChunkStatus& taskStatus = task->getTargetStatus();
        needsReschedule = status.isAfter(taskStatus);
    }
    if (needsReschedule) {
        rescheduleChunkTask(scheduler, &status);
    }

    return future;
}

GenerationChunkHolder::FutureType GenerationChunkHolder::applyStep(
    const world::chunk::status::ChunkStep& step,
    GeneratingChunkMap& chunkMap,
    util::StaticCache2D<GenerationChunkHolder*>& cache
) {
    // Reference: GenerationChunkHolder.java lines 66-81
    if (isStatusDisallowed(step.targetStatus())) {
        return UNLOADED_CHUNK_FUTURE;
    }

    if (acquireStatusBump(step.targetStatus())) {
        // Call the chunk map to apply the step
        auto chunkFuture = chunkMap.applyStep(this, step, cache);

        // Convert ChunkAccess* future to ChunkResultType future
        // Note: Capture targetStatus by pointer (static lifetime) - NOT by reference
        // Reference: In Java, objects are reference types so this isn't an issue
        const world::chunk::status::ChunkStatus* targetStatus = &step.targetStatus();
        auto resultFuture = std::make_shared<util::CompletableFuture<ChunkResultType>>();
        chunkFuture->thenAccept([this, targetStatus, resultFuture](ChunkAccess* chunk) {
            if (chunk != nullptr) {
                completeFuture(*targetStatus, chunk);
                resultFuture->complete(ChunkResult<ChunkAccess*>::of(chunk));
            } else {
                resultFuture->complete(ChunkResult<ChunkAccess*>::error("Generation failed"));
            }
        });

        return resultFuture;
    } else {
        return getOrCreateFuture(step.targetStatus());
    }
}

void GenerationChunkHolder::updateHighestAllowedStatus(ChunkMap& scheduler) {
    // Reference: GenerationChunkHolder.java lines 83-95
    const world::chunk::status::ChunkStatus* oldStatus =
        m_highestAllowedStatus.load(std::memory_order_acquire);
    const world::chunk::status::ChunkStatus* newStatus =
        ChunkLevel::generationStatus(getTicketLevel());

    m_highestAllowedStatus.store(newStatus, std::memory_order_release);

    bool statusDropped = oldStatus != nullptr &&
        (newStatus == nullptr || newStatus->isBefore(*oldStatus));

    if (statusDropped) {
        failAndClearPendingFuturesBetween(newStatus, *oldStatus);
        bool hasTask;
        {
            std::lock_guard<std::mutex> lock(m_taskMutex);
            hasTask = (m_task != nullptr);
        }
        if (hasTask) {
            rescheduleChunkTask(scheduler, findHighestStatusWithPendingFuture(newStatus));
        }
    }
}

void GenerationChunkHolder::replaceProtoChunk(ChunkAccess* imposterChunk) {
    // Reference: GenerationChunkHolder.java lines 97-113
    auto imposterFuture = util::CompletableFuture<ChunkResultType>::completedFuture(
        ChunkResult<ChunkAccess*>::of(imposterChunk)
    );

    std::lock_guard<std::mutex> lock(m_futuresMutex);
    for (int i = 0; i < STATUS_COUNT - 1; ++i) {
        FutureType future = m_futures[i];

        if (future != nullptr) {
            ChunkResultType result = future->getNow(NOT_DONE_YET);
            ChunkAccess* maybeProtoChunk = result->orElse(nullptr);

            // In Java: check if instanceof ProtoChunk
            // For now, just verify it's not null
            if (maybeProtoChunk == nullptr) {
                throw std::logic_error("Trying to replace a ProtoChunk, but found null");
            }

            m_futures[i] = imposterFuture;
        }
    }
}

void GenerationChunkHolder::removeTask(ChunkGenerationTask* task) {
    // Reference: GenerationChunkHolder.java lines 115-117
    // Java: this.task.compareAndSet(task, null);
    std::lock_guard<std::mutex> lock(m_taskMutex);
    if (m_task.get() == task) {
        m_task = nullptr;
    }
}

void GenerationChunkHolder::rescheduleChunkTask(
    ChunkMap& scheduler,
    const world::chunk::status::ChunkStatus* status
) {
    // Reference: GenerationChunkHolder.java lines 119-132
    std::shared_ptr<ChunkGenerationTask> newTask = nullptr;
    if (status != nullptr) {
        newTask = scheduler.scheduleGenerationTask(*status, getPos());
    }

    std::shared_ptr<ChunkGenerationTask> oldTask;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        oldTask = m_task;
        m_task = newTask;
    }
    if (oldTask != nullptr) {
        oldTask->markForCancellation();
    }
}

GenerationChunkHolder::FutureType GenerationChunkHolder::getOrCreateFuture(
    const world::chunk::status::ChunkStatus& status
) {
    // Reference: GenerationChunkHolder.java lines 134-156
    if (isStatusDisallowed(status)) {
        return UNLOADED_CHUNK_FUTURE;
    }

    int index = status.getIndex();

    std::lock_guard<std::mutex> lock(m_futuresMutex);
    FutureType future = m_futures[index];

    if (future == nullptr) {
        auto newFuture = std::make_shared<util::CompletableFuture<ChunkResultType>>();
        m_futures[index] = newFuture;

        if (isStatusDisallowed(status)) {
            failAndClearPendingFutureUnlocked(index, newFuture);
            return UNLOADED_CHUNK_FUTURE;
        }
        return newFuture;
    }

    return future;
}

void GenerationChunkHolder::failAndClearPendingFuturesBetween(
    const world::chunk::status::ChunkStatus* fromExclusive,
    const world::chunk::status::ChunkStatus& toInclusive
) {
    // Reference: GenerationChunkHolder.java lines 158-169
    int start = (fromExclusive == nullptr) ? 0 : fromExclusive->getIndex() + 1;
    int end = toInclusive.getIndex();

    std::lock_guard<std::mutex> lock(m_futuresMutex);
    for (int i = start; i <= end; ++i) {
        FutureType previous = m_futures[i];
        if (previous != nullptr) {
            failAndClearPendingFutureUnlocked(i, previous);
        }
    }
}

void GenerationChunkHolder::failAndClearPendingFuture(int index, FutureType& previous) {
    std::lock_guard<std::mutex> lock(m_futuresMutex);
    failAndClearPendingFutureUnlocked(index, previous);
}

void GenerationChunkHolder::failAndClearPendingFutureUnlocked(int index, FutureType& previous) {
    // Reference: GenerationChunkHolder.java lines 171-175
    // Must be called with m_futuresMutex held
    if (previous->complete(UNLOADED_CHUNK)) {
        m_futures[index] = nullptr;
    }
}

void GenerationChunkHolder::completeFuture(
    const world::chunk::status::ChunkStatus& status,
    ChunkAccess* chunk
) {
    // Reference: GenerationChunkHolder.java lines 177-199
    ChunkResultType result = ChunkResult<ChunkAccess*>::of(chunk);
    int index = status.getIndex();

    std::lock_guard<std::mutex> lock(m_futuresMutex);

    FutureType future = m_futures[index];

    if (future == nullptr) {
        auto completedFuture = util::CompletableFuture<ChunkResultType>::completedFuture(result);
        m_futures[index] = completedFuture;
        return;
    }

    if (future->complete(result)) {
        return;
    }

    ChunkResultType currentResult = future->getNow(NOT_DONE_YET);
    if (currentResult->isSuccess()) {
        throw std::logic_error("Trying to complete a future but found it to be completed successfully already");
    }
}

const world::chunk::status::ChunkStatus* GenerationChunkHolder::findHighestStatusWithPendingFuture(
    const world::chunk::status::ChunkStatus* newStatus
) {
    // Reference: GenerationChunkHolder.java lines 201-219
    if (newStatus == nullptr) {
        return nullptr;
    }

    const world::chunk::status::ChunkStatus* highestStatus = newStatus;
    const world::chunk::status::ChunkStatus* alreadyStarted =
        m_startedWork.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(m_futuresMutex);
    while (alreadyStarted == nullptr || highestStatus->isAfter(*alreadyStarted)) {
        if (m_futures[highestStatus->getIndex()] != nullptr) {
            return highestStatus;
        }

        if (highestStatus == &world::chunk::status::ChunkStatus::EMPTY) {
            break;
        }

        highestStatus = &highestStatus->getParent();
    }

    return nullptr;
}

bool GenerationChunkHolder::acquireStatusBump(const world::chunk::status::ChunkStatus& status) {
    // Reference: GenerationChunkHolder.java lines 221-232
    const world::chunk::status::ChunkStatus* parent =
        (&status == &world::chunk::status::ChunkStatus::EMPTY) ? nullptr : &status.getParent();

    const world::chunk::status::ChunkStatus* previousStarted = parent;
    if (m_startedWork.compare_exchange_strong(previousStarted,
        const_cast<world::chunk::status::ChunkStatus*>(&status))) {
        return true;
    }

    if (previousStarted != nullptr && !status.isAfter(*previousStarted)) {
        return false;
    }

    throw std::logic_error("Unexpected last startedWork status while trying to start");
}

bool GenerationChunkHolder::isStatusDisallowed(const world::chunk::status::ChunkStatus& status) const {
    // Reference: GenerationChunkHolder.java lines 234-237
    const world::chunk::status::ChunkStatus* highestAllowedStatus =
        m_highestAllowedStatus.load(std::memory_order_acquire);
    return highestAllowedStatus == nullptr || status.isAfter(*highestAllowedStatus);
}

void GenerationChunkHolder::increaseGenerationRefCount() {
    // Reference: GenerationChunkHolder.java lines 241-247
    if (m_generationRefCount.fetch_add(1, std::memory_order_acq_rel) == 0) {
        m_generationSaveSyncFuture = std::make_shared<util::CompletableFuture<void>>();
        addSaveDependency(m_generationSaveSyncFuture);
    }
}

void GenerationChunkHolder::decreaseGenerationRefCount() {
    // Reference: GenerationChunkHolder.java lines 249-259
    auto future = m_generationSaveSyncFuture;
    int newValue = m_generationRefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;

    if (newValue == 0) {
        future->complete();
    }

    if (newValue < 0) {
        throw std::logic_error("More releases than claims. Count: " + std::to_string(newValue));
    }
}

GenerationChunkHolder::ChunkAccess* GenerationChunkHolder::getChunkIfPresentUnchecked(
    const world::chunk::status::ChunkStatus& status
) const {
    // Reference: GenerationChunkHolder.java lines 261-264
    std::lock_guard<std::mutex> lock(m_futuresMutex);
    FutureType future = m_futures[status.getIndex()];
    if (future == nullptr) {
        return nullptr;
    }
    ChunkResultType result = future->getNow(NOT_DONE_YET);
    return result->orElse(nullptr);
}

GenerationChunkHolder::ChunkAccess* GenerationChunkHolder::getChunkIfPresent(
    const world::chunk::status::ChunkStatus& status
) const {
    // Reference: GenerationChunkHolder.java lines 266-268
    if (isStatusDisallowed(status)) {
        return nullptr;
    }
    return getChunkIfPresentUnchecked(status);
}

GenerationChunkHolder::ChunkAccess* GenerationChunkHolder::getLatestChunk() const {
    // Reference: GenerationChunkHolder.java lines 270-278
    const world::chunk::status::ChunkStatus* status =
        m_startedWork.load(std::memory_order_acquire);

    if (status == nullptr) {
        return nullptr;
    }

    ChunkAccess* chunk = getChunkIfPresentUnchecked(*status);
    if (chunk != nullptr) {
        return chunk;
    }

    return getChunkIfPresentUnchecked(status->getParent());
}

const world::chunk::status::ChunkStatus* GenerationChunkHolder::getPersistedStatus() const {
    // Reference: GenerationChunkHolder.java lines 280-284
    std::lock_guard<std::mutex> lock(m_futuresMutex);
    FutureType future = m_futures[world::chunk::status::ChunkStatus::EMPTY.getIndex()];

    if (future == nullptr) {
        return nullptr;
    }

    ChunkResultType result = future->getNow(NOT_DONE_YET);
    ChunkAccess* chunkAccess = result->orElse(nullptr);
    if (chunkAccess == nullptr) {
        return nullptr;
    }

    return chunkAccess->getPersistedStatus();
}

FullChunkStatus GenerationChunkHolder::getFullStatus() const {
    // Reference: GenerationChunkHolder.java lines 290-292
    return ChunkLevel::fullStatus(getTicketLevel());
}

std::vector<std::pair<const world::chunk::status::ChunkStatus*, GenerationChunkHolder::FutureType>>
GenerationChunkHolder::getAllFutures() const {
    // Reference: GenerationChunkHolder.java lines 299-307
    std::vector<std::pair<const world::chunk::status::ChunkStatus*, FutureType>> result;
    result.reserve(STATUS_COUNT);

    auto statusList = world::chunk::status::ChunkStatus::getStatusList();  // Copy, not ref
    std::lock_guard<std::mutex> lock(m_futuresMutex);
    for (int i = 0; i < STATUS_COUNT && i < static_cast<int>(statusList.size()); ++i) {
        result.emplace_back(statusList[i], m_futures[i]);
    }

    return result;
}

const world::chunk::status::ChunkStatus* GenerationChunkHolder::getLatestStatus() const {
    // Reference: GenerationChunkHolder.java lines 310-318
    const world::chunk::status::ChunkStatus* status =
        m_startedWork.load(std::memory_order_acquire);

    if (status == nullptr) {
        return nullptr;
    }

    ChunkAccess* chunk = getChunkIfPresentUnchecked(*status);
    return (chunk != nullptr) ? status : &status->getParent();
}

} // namespace level
} // namespace server
} // namespace minecraft
