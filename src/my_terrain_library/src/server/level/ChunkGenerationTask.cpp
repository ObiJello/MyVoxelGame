#include "server/level/ChunkGenerationTask.h"
#include "util/TerrainProfiling.h"
#include "server/level/GeneratingChunkMap.h"
#include "server/level/ChunkMap.h"
#include "world/chunk/status/ChunkPyramid.h"
#include "world/chunk/status/ChunkDependencies.h"

// Reference: net/minecraft/server/level/ChunkGenerationTask.java

namespace minecraft {
namespace server {
namespace level {

using ChunkStatus = world::chunk::status::ChunkStatus;
using ChunkPyramid = world::chunk::status::ChunkPyramid;

ChunkGenerationTask::ChunkGenerationTask(
    GeneratingChunkMap& chunkMap,
    const ChunkStatus& targetStatus,
    const world::ChunkPos& pos,
    util::StaticCache2D<GenerationChunkHolder*> cache
)
    : m_chunkMap(&chunkMap)
    , m_pos(pos)
    , m_targetStatus(targetStatus)
    , m_cache(std::move(cache))
{
    // Reference: ChunkGenerationTask.java lines 27-32
}

std::shared_ptr<ChunkGenerationTask> ChunkGenerationTask::create(
    GeneratingChunkMap& chunkMap,
    const ChunkStatus& targetStatus,
    const world::ChunkPos& pos
) {
    // Reference: ChunkGenerationTask.java lines 34-38
    int worstCaseRadius = ChunkPyramid::getGenerationPyramid()
        .getStepTo(targetStatus)
        .getAccumulatedRadiusOf(ChunkStatus::EMPTY);

    auto cache = util::StaticCache2D<GenerationChunkHolder*>::create(
        pos.x(), pos.z(), worstCaseRadius,
        [&chunkMap](int x, int z) {
            return chunkMap.acquireGeneration(world::ChunkPos::asLong(x, z));
        }
    );

    return std::shared_ptr<ChunkGenerationTask>(
        new ChunkGenerationTask(chunkMap, targetStatus, pos, std::move(cache))
    );
}

ChunkGenerationTask::FutureType ChunkGenerationTask::runUntilWait() {
    if (!m_started.exchange(true, std::memory_order_acq_rel) &&
        m_markedForCancellation.load(std::memory_order_acquire)) {
        releaseClaim();
        return nullptr;
    }
    if (m_claimReleased.load(std::memory_order_acquire)) return nullptr;   // cancelled before it started
    // Reference: ChunkGenerationTask.java lines 40-54
    while (true) {
        auto waitingFor = waitForScheduledLayer();
        if (waitingFor != nullptr) {
            return waitingFor;
        }

        if (m_markedForCancellation.load(std::memory_order_acquire) ||
            m_scheduledStatus == &m_targetStatus) {
            releaseClaim();
            return nullptr;
        }

        scheduleNextLayer();
    }
}

void ChunkGenerationTask::scheduleNextLayer() {
    // Reference: ChunkGenerationTask.java lines 56-69
    const ChunkStatus* statusToSchedule;

    if (m_scheduledStatus == nullptr) {
        statusToSchedule = &ChunkStatus::EMPTY;
    } else if (!m_needsGeneration &&
               m_scheduledStatus == &ChunkStatus::EMPTY &&
               !canLoadWithoutGeneration()) {
        m_needsGeneration = true;
        statusToSchedule = &ChunkStatus::EMPTY;
    } else {
        // Move to next status
        const auto& statusList = ChunkStatus::getStatusList();
        int nextIndex = m_scheduledStatus->getIndex() + 1;
        if (nextIndex < static_cast<int>(statusList.size())) {
            statusToSchedule = statusList[nextIndex];
        } else {
            statusToSchedule = m_scheduledStatus;
        }
    }

    scheduleLayer(*statusToSchedule, m_needsGeneration);
    m_scheduledStatus = statusToSchedule;
}

void ChunkGenerationTask::markForCancellation() {
    // Reference: ChunkGenerationTask.java lines 71-73
    m_markedForCancellation.store(true, std::memory_order_release);
    // A task that never started (still queued, lowest priority after its
    // area was left) would only drop its generation refs when the dispatcher
    // finally ran it — after every nearer task. Meanwhile ~290 holders per
    // task stayed pinned and the old area could not unload (measured
    // 2026-08-30: 15k holders after a teleport). Release now if no run has
    // claimed the task; a running task keeps ownership and releases itself.
    // Early release for never-started tasks was tried here (2026-08-30) and
    // is off: it only matters when holder unloading is on, and that path is
    // disabled until its stuck-task race is understood.
}

void ChunkGenerationTask::releaseClaim() {
    // Reference: ChunkGenerationTask.java lines 75-82
    if (m_claimReleased.exchange(true, std::memory_order_acq_rel)) return;   // idempotent
    GenerationChunkHolder* chunkHolder = m_cache.get(m_pos.x(), m_pos.z());
    chunkHolder->removeTask(this);

    m_cache.forEach([this](GenerationChunkHolder* holder) {
        m_chunkMap->releaseGeneration(holder);
    });
}

bool ChunkGenerationTask::canLoadWithoutGeneration() {
    // Reference: ChunkGenerationTask.java lines 84-109
    if (&m_targetStatus == &ChunkStatus::EMPTY) {
        return true;
    }
    if (m_cache.get(m_pos.x(), m_pos.z())->forceGeneration()) {
        return false;   // a previous attempt found the loading shortcut inconsistent
    }

    const ChunkStatus* highestGeneratedStatus = m_cache.get(m_pos.x(), m_pos.z())->getPersistedStatus();

    if (highestGeneratedStatus != nullptr && !highestGeneratedStatus->isBefore(m_targetStatus)) {
        const auto& dependencies = ChunkPyramid::getLoadingPyramid()
            .getStepTo(m_targetStatus)
            .accumulatedDependencies();

        int range = dependencies.getRadius();

        for (int x = m_pos.x() - range; x <= m_pos.x() + range; ++x) {
            for (int z = m_pos.z() - range; z <= m_pos.z() + range; ++z) {
                int distance = m_pos.getChessboardDistance(x, z);
                const ChunkStatus& requiredStatus = dependencies.get(distance);
                const ChunkStatus* persistedStatus = m_cache.get(x, z)->getPersistedStatus();

                if (persistedStatus == nullptr || persistedStatus->isBefore(requiredStatus)) {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}

GenerationChunkHolder* ChunkGenerationTask::getCenter() {
    // Reference: ChunkGenerationTask.java lines 111-113
    return m_cache.get(m_pos.x(), m_pos.z());
}

void ChunkGenerationTask::scheduleLayer(
    const ChunkStatus& status,
    bool needsGeneration
) {
    TERRAIN_ZONE_N("Lane.ScheduleLayer");
    // Reference: ChunkGenerationTask.java lines 115-131
    int radius = getRadiusForLayer(status, needsGeneration);

    for (int x = m_pos.x() - radius; x <= m_pos.x() + radius; ++x) {
        for (int z = m_pos.z() - radius; z <= m_pos.z() + radius; ++z) {
            GenerationChunkHolder* chunkHolder = m_cache.get(x, z);

            if (m_markedForCancellation.load(std::memory_order_acquire) ||
                !scheduleChunkInLayer(status, needsGeneration, chunkHolder)) {
                return;
            }
        }
    }
}

int ChunkGenerationTask::getRadiusForLayer(
    const ChunkStatus& status,
    bool needsGeneration
) {
    // Reference: ChunkGenerationTask.java lines 133-136
    const ChunkPyramid& pyramid = needsGeneration
        ? ChunkPyramid::getGenerationPyramid()
        : ChunkPyramid::getLoadingPyramid();

    return pyramid.getStepTo(m_targetStatus).getAccumulatedRadiusOf(status);
}

bool ChunkGenerationTask::scheduleChunkInLayer(
    const ChunkStatus& status,
    bool needsGeneration,
    GenerationChunkHolder* chunkHolder
) {
    // Reference: ChunkGenerationTask.java lines 138-157
    const ChunkStatus* persistedStatus = chunkHolder->getPersistedStatus();
    bool generate = (persistedStatus != nullptr && status.isAfter(*persistedStatus));

    const ChunkPyramid& pyramid = generate
        ? ChunkPyramid::getGenerationPyramid()
        : ChunkPyramid::getLoadingPyramid();

    if (generate && !needsGeneration) {
        // Java throws IllegalStateException here: its saved statuses and its
        // pyramid can never disagree. Ours can — the game saves FULL chunks
        // to the region files the library loads from, while the library's
        // own ring chunks stay unsaved — so a task that chose the loading
        // pyramid can meet a neighbour that still needs generating. Recover:
        // flag the centre so the NEXT task takes the generation pyramid from
        // the start, and reschedule it now (rescheduleChunkTask cancels this
        // task; runUntilWait then releases its claims). Restarting in place
        // was tried and skipped statuses on already-bumped holders.
        GenerationChunkHolder* center = getCenter();
        center->setForceGeneration();
        if (auto* map = dynamic_cast<ChunkMap*>(m_chunkMap)) {
            center->rescheduleChunkTask(*map, &m_targetStatus);
        } else {
            markForCancellation();
        }
        return false;
    }

    // Already at or past this layer's status: the step's future is done and
    // successful (the status is written by the step's thenApply just before
    // the holder completes it), so applyStep would only take the holder's
    // mutex to discover that. Skipping it is what makes the two 529-holder
    // layers of a FULL pyramid cheap; Java's volatile reads make it free.
    if (persistedStatus != nullptr && !status.isAfter(*persistedStatus)) {
        return true;
    }

    FutureType future = chunkHolder->applyStep(
        pyramid.getStepTo(status),
        *m_chunkMap,
        m_cache
    );

    ChunkResultType now = future->getNow(nullptr);

    if (now == nullptr) {
        m_scheduledLayer.push_back(future);
        m_scheduledLayerInfo.emplace_back(chunkHolder->getPos().toLong(), status.getIndex());
        return true;
    } else if (now->isSuccess()) {
        return true;
    } else {
        markForCancellation();
        return false;
    }
}

ChunkGenerationTask::FutureType ChunkGenerationTask::waitForScheduledLayer() {
    // Reference: ChunkGenerationTask.java lines 159-174
    while (!m_scheduledLayer.empty()) {
        FutureType lastFuture = m_scheduledLayer.back();
        ChunkResultType resultNow = lastFuture->getNow(nullptr);

        if (resultNow == nullptr) {
            return lastFuture;
        }

        m_scheduledLayer.pop_back();
        m_scheduledLayerInfo.pop_back();
        if (!resultNow->isSuccess()) {
            markForCancellation();
        }
    }

    return nullptr;
}

} // namespace level
} // namespace server
} // namespace minecraft
