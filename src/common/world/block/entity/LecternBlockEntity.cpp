// File: src/common/world/block/entity/LecternBlockEntity.cpp
#include "LecternBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/BookItems.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/LecternBlock.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        // MC Mth.clamp(int, int, int): min(max(value, min), max) — max wins
        // when the range is empty (a book with no pages clamps to -1).
        int ClampInt(int value, int lo, int hi) { return std::min(std::max(value, lo), hi); }

        // MC LecternBlockEntity.resolveBook: only a server level resolves.
        void ResolveBook(ILevelWrite* level, ItemStack& book, const std::string* readerName) {
            if (level && level->IsClientSide()) return;
            // MC's command source without a player is named "Lectern".
            Books::ResolveForItem(book, readerName ? *readerName : std::string("Lectern"));
        }
    } // namespace

    bool LecternBlockEntity::HasBook() const {
        return m_book.get(DataComponents::WRITABLE_BOOK_CONTENT).has_value() ||
               m_book.get(DataComponents::WRITTEN_BOOK_CONTENT).has_value();
    }

    void LecternBlockEntity::SetChanged() {
        MarkDirty();
        if (ILevelWrite* level = GetLevel()) {
            level->BlockEntityChanged(GetWorldPos());
            if (BlockRegistry::Get(GetBlockId()).hasAnalogOutputSignal) {
                level->UpdateNeighbourForOutputSignal(GetWorldPos(), GetBlockId());
            }
        }
    }

    void LecternBlockEntity::SetBook(ItemStack book, const std::string* readerName) {
        ResolveBook(GetLevel(), book, readerName);
        m_book = std::move(book);
        m_page = 0;
        m_pageCount = Books::PageCount(m_book);
        SetChanged();
    }

    void LecternBlockEntity::LoadFromNbt(ItemStack book, int page) {
        // MC loadAdditional: the level is not installed yet when a chunk is
        // read, so resolveBook runs with whatever level there is (none) —
        // vanilla's `this.level instanceof ServerLevel` is false at load too.
        m_book = std::move(book);
        m_pageCount = Books::PageCount(m_book);
        m_page = ClampInt(page, 0, m_pageCount - 1);
    }

    void LecternBlockEntity::SetPage(int page) {
        const int newPage = ClampInt(page, 0, m_pageCount - 1);
        if (newPage == m_page) return;
        m_page = newPage;
        SetChanged();
        if (ILevelWrite* level = GetLevel()) {
            LecternSignalPageChange(*level, GetWorldPos(),
                                    level->GetBlockState(GetWorldPos().x, GetWorldPos().y, GetWorldPos().z));
        }
    }

    int LecternBlockEntity::GetRedstoneSignal() const {
        const float progress = m_pageCount > 1
            ? static_cast<float>(GetPage()) / (static_cast<float>(m_pageCount) - 1.0f)
            : 1.0f;
        return static_cast<int>(std::floor(progress * 14.0f)) + (HasBook() ? 1 : 0);
    }

    ItemStack LecternBlockEntity::RemoveBook() {
        ItemStack previous = std::move(m_book);
        m_book = ItemStack{};
        // MC onBookItemRemove.
        m_page = 0;
        m_pageCount = 0;
        SetChanged();
        if (ILevelWrite* level = GetLevel()) {
            const glm::ivec3& p = GetWorldPos();
            const BlockState state = level->GetBlockState(p.x, p.y, p.z);
            if (state.Block() == BlockID::Lectern) LecternResetBookState(*level, p, state, false);
        }
        return previous;
    }

    void LecternBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos,
                                                  BlockState oldState) {
        if (level.IsClientSide() || !BoolOf(oldState, PropertyId::HAS_BOOK)) return;
        if (m_book.IsEmpty()) return;
        const glm::ivec3 step = Relative(glm::ivec3(0), HorizontalFacingOf(oldState));
        const double xo = 0.25 * static_cast<double>(step.x);
        const double zo = 0.25 * static_cast<double>(step.z);
        // new ItemEntity(level, x, y, z, book): velocity
        // (nextDouble*0.2-0.1, 0.2, nextDouble*0.2-0.1), then
        // setDefaultPickUpDelay (10 ticks).
        JavaRandom* random = level.Random();
        const double vx = random ? random->NextDouble() * 0.2 - 0.1 : 0.0;
        const double vz = random ? random->NextDouble() * 0.2 - 0.1 : 0.0;
        SpawnItemEntity(level.GetDimension(),
                        glm::dvec3(pos.x + 0.5 + xo, pos.y + 1.0, pos.z + 0.5 + zo),
                        glm::dvec3(vx, 0.2, vz), m_book, 10);
    }

} // namespace Game
