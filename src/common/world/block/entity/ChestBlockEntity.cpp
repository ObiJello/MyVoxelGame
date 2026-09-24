// File: src/common/world/block/entity/ChestBlockEntity.cpp
//
// The chest lid: MC ContainerOpenersCounter + ChestLidController as used by
// ChestBlockEntity, TrappedChestBlockEntity and EnderChestBlockEntity (one
// class here — see the header). Server half: count users, book block event 1
// with the count, recheck every 5 ticks. Client half: the lid eases toward
// the count's verdict in ClientTick.
#include "ChestBlockEntity.hpp"

#include "DoubleChest.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <atomic>

namespace Game {

    namespace {
        // MC ContainerOpenersCounter.CHECK_TICK_DELAY.
        constexpr int kCheckTickDelay = 5;

        std::atomic<ChestBlockEntity::UserCounter> s_userCounter{nullptr};
    }

    void ChestBlockEntity::SetUserCounter(UserCounter counter) {
        s_userCounter.store(counter, std::memory_order_release);
    }

    // MC ContainerOpenersCounter.incrementOpeners.
    void ChestBlockEntity::StartOpen(ILevelWrite& level) {
        const int previous = m_openCount++;
        if (previous == 0) {
            OnOpen(level);
            ScheduleRecheck(level);
        }
        OpenerCountChanged(level, previous, m_openCount);
    }

    // MC ContainerOpenersCounter.decrementOpeners. The floor at zero is not
    // MC's: there a stopOpen always pairs a startOpen on the same object,
    // while here a recheck may already have settled the count to zero before
    // the matching close arrives.
    void ChestBlockEntity::StopOpen(ILevelWrite& level) {
        if (m_openCount <= 0) return;
        const int previous = m_openCount--;
        if (m_openCount == 0) OnClose(level);
        OpenerCountChanged(level, previous, m_openCount);
    }

    // MC ContainerOpenersCounter.recheckOpeners.
    void ChestBlockEntity::RecheckOpen(ILevelWrite& level) {
        const UserCounter counter = s_userCounter.load(std::memory_order_acquire);
        const int openCount = counter ? counter(level, GetWorldPos()) : m_openCount;
        const int prevCount = m_openCount;
        if (prevCount != openCount) {
            const bool isOpen  = openCount != 0;
            const bool wasOpen = prevCount != 0;
            if (isOpen && !wasOpen) {
                OnOpen(level);
            } else if (!isOpen) {
                OnClose(level);
            }
            m_openCount = openCount;
        }
        OpenerCountChanged(level, prevCount, openCount);
        if (openCount > 0) ScheduleRecheck(level);
    }

    void ChestBlockEntity::ScheduleRecheck(ILevelWrite& level) {
        // MC scheduleRecheck: level.scheduleTick(pos, block, 5) — the block's
        // tick (ChestBlock / EnderChestBlock.tick) calls RecheckOpen.
        if (ScheduledTickAccess* ticks = level.Ticks()) {
            ticks->ScheduleTick(GetWorldPos(), GetBlockId(), kCheckTickDelay);
        }
    }

    void ChestBlockEntity::OnOpen(ILevelWrite& level)  { PlayLidSound(level, true); }
    void ChestBlockEntity::OnClose(ILevelWrite& level) { PlayLidSound(level, false); }

    // MC ChestBlockEntity.signalOpenCount (+ TrappedChestBlockEntity's
    // override): the count goes to every watching client as block event 1,
    // and a trapped chest's redstone output (its opener count) re-reads.
    void ChestBlockEntity::OpenerCountChanged(ILevelWrite& level, int previous, int current) {
        level.BlockEvent(GetWorldPos(), GetBlockId(), kEventSetOpenCount, current);
        if (GetBlockId() == BlockID::TrappedChest && previous != current) {
            const glm::ivec3 pos = GetWorldPos();
            level.UpdateNeighborsAt(pos, BlockID::TrappedChest);
            level.UpdateNeighborsAt(pos + glm::ivec3(0, -1, 0), BlockID::TrappedChest);
        }
    }

    // MC ChestBlockEntity.playSound / EnderChestBlockEntity's onOpen/onClose.
    // A double chest sounds once, from its seam: the LEFT half (MC's second
    // chest) is silent and the RIGHT half plays half a block toward it.
    void ChestBlockEntity::PlayLidSound(ILevelWrite& level, bool open) {
        const glm::ivec3 pos = GetWorldPos();
        glm::dvec3 at = glm::dvec3(pos) + glm::dvec3(0.5);
        const bool ender = GetBlockId() == BlockID::EnderChest;
        if (!ender) {
            if (auto pair = FindChestPartner(level, pos)) {
                if (!pair->selfIsFirst) return;   // ChestType.LEFT
                const glm::ivec3 step = pair->partnerPos - pos;
                at.x += static_cast<double>(step.x) * 0.5;
                at.z += static_cast<double>(step.z) * 0.5;
            }
        }
        const char* event = ender ? (open ? SoundEvents::ENDER_CHEST_OPEN : SoundEvents::ENDER_CHEST_CLOSE)
                                  : (open ? SoundEvents::CHEST_OPEN : SoundEvents::CHEST_CLOSE);
        JavaRandom* random = level.Random();
        const float pitch = random ? random->NextFloat() * 0.1f + 0.9f : 1.0f;
        // MC: level.playSound(null, x, y, z, sound, BLOCKS, 0.5F, pitch).
        level.PlaySound(nullptr, at, event, SoundSource::Blocks, 0.5f, pitch);
    }

    // MC ChestBlockEntity.triggerEvent.
    bool ChestBlockEntity::TriggerEvent(int b0, int b1) {
        if (b0 == kEventSetOpenCount) {
            m_lid.ShouldBeOpen(b1 > 0);
            return true;
        }
        return BaseContainerBlockEntity::TriggerEvent(b0, b1);
    }

} // namespace Game
