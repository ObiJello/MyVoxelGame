#include "server/level/ChunkHolder.h"
#include "server/level/ChunkLevel.h"
#include "server/level/ChunkMap.h"

// Reference: net/minecraft/server/level/ChunkHolder.java

namespace minecraft {
namespace server {
namespace level {

// Static member definitions
ChunkHolder::LevelChunkResult ChunkHolder::UNLOADED_LEVEL_CHUNK;
ChunkHolder::LevelChunkFuture ChunkHolder::UNLOADED_LEVEL_CHUNK_FUTURE;

void ChunkHolder::initializeStatics() {
    // Reference: ChunkHolder.java lines 32-33
    UNLOADED_LEVEL_CHUNK = ChunkResult<LevelChunk*>::error("Unloaded level chunk");
    UNLOADED_LEVEL_CHUNK_FUTURE = util::CompletableFuture<LevelChunkResult>::completedFuture(UNLOADED_LEVEL_CHUNK);
}

ChunkHolder::ChunkHolder(
    const world::ChunkPos& pos,
    int ticketLevel,
    int levelHeight,
    int minY,
    LevelChangeListener onLevelChange,
    PlayerProvider playerProvider
)
    : GenerationChunkHolder(pos)
    , m_levelHeight(levelHeight)
    , m_minY(minY)
    , m_sectionsCount((levelHeight + 15) / 16)  // Sections are 16 blocks tall
    , m_fullChunkFuture(UNLOADED_LEVEL_CHUNK_FUTURE)
    , m_tickingChunkFuture(UNLOADED_LEVEL_CHUNK_FUTURE)
    , m_entityTickingChunkFuture(UNLOADED_LEVEL_CHUNK_FUTURE)
    , m_oldTicketLevel(ChunkLevel::getMaxLevel() + 1)
    , m_ticketLevel(ChunkLevel::getMaxLevel() + 1)
    , m_queueLevel(ChunkLevel::getMaxLevel() + 1)
    , m_onLevelChange(std::move(onLevelChange))
    , m_playerProvider(std::move(playerProvider))
    , m_pendingFullStateConfirmation(util::CompletableFuture<void>::completedFuture())
    , m_sendSync(util::CompletableFuture<void>::completedFuture())
    , m_saveSync(util::CompletableFuture<void>::completedFuture())
{
    // Reference: ChunkHolder.java lines 53-72
    m_changedBlocksPerSection.resize(m_sectionsCount);
    setTicketLevel(ticketLevel);
}

ChunkHolder::LevelChunkFuture ChunkHolder::getTickingChunkFuture() {
    // Reference: ChunkHolder.java lines 74-76
    return m_tickingChunkFuture;
}

ChunkHolder::LevelChunkFuture ChunkHolder::getEntityTickingChunkFuture() {
    // Reference: ChunkHolder.java lines 78-80
    return m_entityTickingChunkFuture;
}

ChunkHolder::LevelChunkFuture ChunkHolder::getFullChunkFuture() {
    // Reference: ChunkHolder.java lines 82-84
    return m_fullChunkFuture;
}

LevelChunk* ChunkHolder::getTickingChunk() {
    // Reference: ChunkHolder.java lines 86-88
    LevelChunkResult result = getTickingChunkFuture()->getNow(UNLOADED_LEVEL_CHUNK);
    return result->orElse(nullptr);
}

LevelChunk* ChunkHolder::getChunkToSend() {
    // Reference: ChunkHolder.java lines 90-92
    if (!m_sendSync->isDone()) {
        return nullptr;
    }
    return getTickingChunk();
}

std::shared_ptr<util::CompletableFuture<void>> ChunkHolder::getSendSyncFuture() {
    // Reference: ChunkHolder.java lines 94-96
    return m_sendSync;
}

void ChunkHolder::addSendDependency(std::shared_ptr<util::CompletableFuture<void>> sync) {
    // Reference: ChunkHolder.java lines 98-105
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sendSync->isDone()) {
        m_sendSync = sync;
    } else {
        // Combine futures
        auto oldSync = m_sendSync;
        m_sendSync = std::make_shared<util::CompletableFuture<void>>();
        auto newSync = m_sendSync;

        oldSync->thenRun([sync, newSync]() {
            sync->whenComplete([newSync](std::exception_ptr) {
                newSync->complete();
            });
        });
    }
}

std::shared_ptr<util::CompletableFuture<void>> ChunkHolder::getSaveSyncFuture() {
    // Reference: ChunkHolder.java lines 107-109
    return m_saveSync;
}

bool ChunkHolder::isReadyForSaving() const {
    // Reference: ChunkHolder.java lines 111-113
    return m_saveSync->isDone();
}

void ChunkHolder::addSaveDependency(std::shared_ptr<util::CompletableFuture<void>> sync) {
    // Reference: ChunkHolder.java lines 115-122
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_saveSync->isDone()) {
        m_saveSync = sync;
    } else {
        // Combine futures
        auto oldSync = m_saveSync;
        m_saveSync = std::make_shared<util::CompletableFuture<void>>();
        auto newSync = m_saveSync;

        oldSync->thenRun([sync, newSync]() {
            sync->whenComplete([newSync](std::exception_ptr) {
                newSync->complete();
            });
        });
    }
}

bool ChunkHolder::blockChanged(int x, int y, int z) {
    // Reference: ChunkHolder.java lines 124-141
    LevelChunk* chunk = getTickingChunk();
    if (chunk == nullptr) {
        return false;
    }

    bool hadChangedSections = m_hasChangedSections.load(std::memory_order_acquire);
    int sectionIndex = getSectionIndex(y);

    if (sectionIndex < 0 || sectionIndex >= m_sectionsCount) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto& changedBlocksInSection = m_changedBlocksPerSection[sectionIndex];
    m_hasChangedSections.store(true, std::memory_order_release);

    // Encode position within section as short
    short relativePos = static_cast<short>(
        ((x & 15) << 8) | ((z & 15) << 4) | (y & 15)
    );
    changedBlocksInSection.insert(relativePos);

    return !hadChangedSections;
}

bool ChunkHolder::sectionLightChanged(int layer, int chunkY) {
    // Reference: ChunkHolder.java lines 143-169
    // Simplified implementation
    std::lock_guard<std::mutex> lock(m_mutex);

    int minLightSection = (m_minY >> 4) - 1;
    int maxLightSection = ((m_minY + m_levelHeight) >> 4);

    if (chunkY < minLightSection || chunkY > maxLightSection) {
        return false;
    }

    int index = chunkY - minLightSection;
    if (index < 0 || index >= 64) {
        return false;
    }

    std::bitset<64>& filter = (layer == 1) ? m_skyChangedLightSectionFilter : m_blockChangedLightSectionFilter;

    if (!filter.test(index)) {
        filter.set(index);
        return true;
    }

    return false;
}

bool ChunkHolder::hasChangesToBroadcast() const {
    // Reference: ChunkHolder.java lines 171-173
    return m_hasChangedSections.load(std::memory_order_acquire) ||
           m_skyChangedLightSectionFilter.any() ||
           m_blockChangedLightSectionFilter.any();
}

int ChunkHolder::getTicketLevel() const {
    // Reference: ChunkHolder.java lines 241-243
    return m_ticketLevel.load(std::memory_order_acquire);
}

int ChunkHolder::getQueueLevel() const {
    // Reference: ChunkHolder.java lines 245-247
    return m_queueLevel.load(std::memory_order_acquire);
}

void ChunkHolder::setQueueLevel(int queueLevel) {
    // Reference: ChunkHolder.java lines 249-251
    m_queueLevel.store(queueLevel, std::memory_order_release);
}

void ChunkHolder::setTicketLevel(int ticketLevel) {
    // Reference: ChunkHolder.java lines 253-255
    m_ticketLevel.store(ticketLevel, std::memory_order_release);
}

void ChunkHolder::scheduleFullChunkPromotion(
    ChunkMap& scheduler,
    LevelChunkFuture task,
    std::function<void(std::function<void()>)> mainThreadExecutor,
    FullChunkStatus status
) {
    // Reference: ChunkHolder.java lines 257-263
    // Cancel previous confirmation
    // Note: C++ CompletableFuture doesn't have cancel(), so we just ignore old ones

    auto confirmation = std::make_shared<util::CompletableFuture<void>>();
    auto pos = m_pos;

    confirmation->thenRun([&scheduler, pos, status, mainThreadExecutor]() {
        mainThreadExecutor([&scheduler, pos, status]() {
            // scheduler.onFullChunkStatusChange(pos, status);
        });
    });

    m_pendingFullStateConfirmation = confirmation;

    task->thenAccept([confirmation](LevelChunkResult r) {
        r->ifSuccess([confirmation](LevelChunk*) {
            confirmation->complete();
        });
    });
}

void ChunkHolder::demoteFullChunk(ChunkMap& scheduler, FullChunkStatus status) {
    // Reference: ChunkHolder.java lines 265-268
    // Cancel confirmation and notify immediately
    // scheduler.onFullChunkStatusChange(m_pos, status);
}

void ChunkHolder::updateFutures(
    ChunkMap& scheduler,
    std::function<void(std::function<void()>)> mainThreadExecutor
) {
    // Reference: ChunkHolder.java lines 270-323
    FullChunkStatus oldFullStatus = ChunkLevel::fullStatus(m_oldTicketLevel);
    FullChunkStatus newFullStatus = ChunkLevel::fullStatus(m_ticketLevel.load());

    bool wasAccessible = isOrAfter(oldFullStatus, FullChunkStatus::FULL);
    bool isAccessible = isOrAfter(newFullStatus, FullChunkStatus::FULL);

    if (isAccessible) {
        m_wasAccessibleSinceLastSave.store(true, std::memory_order_release);
    }

    // Handle FULL status changes
    // Reference: ChunkHolder.java lines 280-285
    if (!wasAccessible && isAccessible) {
        m_fullChunkFuture = scheduler.prepareAccessibleChunk(this);
        scheduleFullChunkPromotion(scheduler, m_fullChunkFuture, mainThreadExecutor, FullChunkStatus::FULL);
        // Create save dependency from the future
        auto saveDep = std::make_shared<util::CompletableFuture<void>>();
        m_fullChunkFuture->thenAccept([saveDep](auto) {
            saveDep->complete();
        });
        addSaveDependency(saveDep);
    }

    if (wasAccessible && !isAccessible) {
        m_fullChunkFuture->complete(UNLOADED_LEVEL_CHUNK);
        m_fullChunkFuture = UNLOADED_LEVEL_CHUNK_FUTURE;
    }

    // Handle BLOCK_TICKING status changes
    bool wasTicking = isOrAfter(oldFullStatus, FullChunkStatus::BLOCK_TICKING);
    bool isTicking = isOrAfter(newFullStatus, FullChunkStatus::BLOCK_TICKING);

    // Reference: ChunkHolder.java lines 294-299
    if (!wasTicking && isTicking) {
        m_tickingChunkFuture = scheduler.prepareTickingChunk(this);
        scheduleFullChunkPromotion(scheduler, m_tickingChunkFuture, mainThreadExecutor, FullChunkStatus::BLOCK_TICKING);
        // Create save dependency from the future
        auto saveDep = std::make_shared<util::CompletableFuture<void>>();
        m_tickingChunkFuture->thenAccept([saveDep](auto) {
            saveDep->complete();
        });
        addSaveDependency(saveDep);
    }

    if (wasTicking && !isTicking) {
        m_tickingChunkFuture->complete(UNLOADED_LEVEL_CHUNK);
        m_tickingChunkFuture = UNLOADED_LEVEL_CHUNK_FUTURE;
    }

    // Handle ENTITY_TICKING status changes
    bool wasEntityTicking = isOrAfter(oldFullStatus, FullChunkStatus::ENTITY_TICKING);
    bool isEntityTicking = isOrAfter(newFullStatus, FullChunkStatus::ENTITY_TICKING);

    // Reference: ChunkHolder.java lines 308-313
    if (!wasEntityTicking && isEntityTicking) {
        m_entityTickingChunkFuture = scheduler.prepareEntityTickingChunk(this);
        scheduleFullChunkPromotion(scheduler, m_entityTickingChunkFuture, mainThreadExecutor, FullChunkStatus::ENTITY_TICKING);
        // Create save dependency from the future
        auto saveDep = std::make_shared<util::CompletableFuture<void>>();
        m_entityTickingChunkFuture->thenAccept([saveDep](auto) {
            saveDep->complete();
        });
        addSaveDependency(saveDep);
    }

    if (wasEntityTicking && !isEntityTicking) {
        m_entityTickingChunkFuture->complete(UNLOADED_LEVEL_CHUNK);
        m_entityTickingChunkFuture = UNLOADED_LEVEL_CHUNK_FUTURE;
    }

    // Handle demotion
    if (!isOrAfter(newFullStatus, oldFullStatus)) {
        demoteFullChunk(scheduler, newFullStatus);
    }

    // Notify level change listener
    auto pos = m_pos;
    int newLevel = m_ticketLevel.load();
    m_onLevelChange(
        pos,
        [this]() { return getQueueLevel(); },
        newLevel,
        [this](int level) { setQueueLevel(level); }
    );

    m_oldTicketLevel = m_ticketLevel.load();
}

bool ChunkHolder::wasAccessibleSinceLastSave() const {
    // Reference: ChunkHolder.java lines 325-327
    return m_wasAccessibleSinceLastSave.load(std::memory_order_acquire);
}

void ChunkHolder::refreshAccessibility() {
    // Reference: ChunkHolder.java lines 329-331
    bool accessible = isOrAfter(
        ChunkLevel::fullStatus(m_ticketLevel.load()),
        FullChunkStatus::FULL
    );
    m_wasAccessibleSinceLastSave.store(accessible, std::memory_order_release);
}

int ChunkHolder::getSectionIndex(int y) const {
    return (y - m_minY) >> 4;  // Divide by 16
}

} // namespace level
} // namespace server
} // namespace minecraft
