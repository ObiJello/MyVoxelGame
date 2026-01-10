#include "server/level/ChunkGenerationTask.h"
#include "server/level/GeneratingChunkMap.h"
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

std::shared_ptr<util::CompletableFuture<void>> ChunkGenerationTask::runUntilWait() {
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
}

void ChunkGenerationTask::releaseClaim() {
    // Reference: ChunkGenerationTask.java lines 75-82
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
        throw std::logic_error("Can't load chunk, but didn't expect to need to generate");
    }

    FutureType future = chunkHolder->applyStep(
        pyramid.getStepTo(status),
        *m_chunkMap,
        m_cache
    );

    ChunkResultType now = future->getNow(nullptr);

    if (now == nullptr) {
        m_scheduledLayer.push_back(future);
        return true;
    } else if (now->isSuccess()) {
        return true;
    } else {
        markForCancellation();
        return false;
    }
}

std::shared_ptr<util::CompletableFuture<void>> ChunkGenerationTask::waitForScheduledLayer() {
    // Reference: ChunkGenerationTask.java lines 159-174
    while (!m_scheduledLayer.empty()) {
        FutureType lastFuture = m_scheduledLayer.back();
        ChunkResultType resultNow = lastFuture->getNow(nullptr);

        if (resultNow == nullptr) {
            // Need to wait - convert to void future
            auto waitFuture = std::make_shared<util::CompletableFuture<void>>();
            lastFuture->thenAccept([waitFuture](ChunkResultType) {
                waitFuture->complete();
            });
            return waitFuture;
        }

        m_scheduledLayer.pop_back();
        if (!resultNow->isSuccess()) {
            markForCancellation();
        }
    }

    return nullptr;
}

} // namespace level
} // namespace server
} // namespace minecraft
