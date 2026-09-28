// File: src/common/world/block/entity/BrushableBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.BrushableBlockEntity — the
// buried find in a suspicious sand / suspicious gravel block (archaeology).
//
//   LootTable / LootTableSeed  what worldgen buried here (archaeology/
//                              desert_pyramid, desert_well, ocean_ruin_*,
//                              trail_ruins_common / _rare). Rolled into
//                              `item` by the first brush stroke
//                              (unpackLootTable, ARCHAEOLOGY params: ORIGIN,
//                              the brusher's luck, the brush as TOOL).
//   item                       the find, drawn poking out of the block by
//                              BrushableBlockRenderer once it is rolled.
//   hit_direction              the face the first stroke landed on — where
//                              the find emerges and where it drops.
//   brushCount + two clocks    ten strokes (one per BRUSH_COOLDOWN_TICKS)
//                              finish the dig; walking away for
//                              BRUSH_RESET_TICKS starts rewinding it two
//                              strokes every 4 ticks (checkReset, run from
//                              the block's scheduled tick). The block's
//                              DUSTED state (0..3) follows the count.
//
// Finishing drops the find out of the brushed face, raises level event 3008
// (the dust burst and brush-complete sound) and turns the block into its
// plain sand / gravel.
//
// Wire (BlockEntityDataS2C — MC getUpdateTag): the hit direction and the
// item, what the renderer needs. Disk (BlockEntityNbt.cpp): LootTable /
// LootTableSeed while unrolled, else `item`; hit_direction.
#pragma once

#include "BlockEntity.hpp"
#include "common/entity/Item.hpp"

#include <cstdint>
#include <string>

namespace Game {

    class ILevelWrite;
    class IUsePlayer;

    class BrushableBlockEntity : public BlockEntity {
    public:
        static constexpr int kBrushCooldownTicks     = 10;   // MC BRUSH_COOLDOWN_TICKS
        static constexpr int kBrushResetTicks        = 40;   // MC BRUSH_RESET_TICKS
        static constexpr int kRequiredBrushesToBreak = 10;   // MC REQUIRED_BRUSHES_TO_BREAK

        BrushableBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // MC brush(gameTime, level, user, direction, brush): one stroke on
        // face `direction` (Direction 3D data value, 0 down .. 5 east).
        // Server only. True when this stroke finished the dig (the brush
        // then takes a point of wear).
        bool Brush(ILevelWrite& level, IUsePlayer* user, int direction);

        // MC checkReset: the block's scheduled tick.
        void CheckReset(ILevelWrite& level);

        // MC setLootTable(key, seed) — worldgen's archaeology tables.
        void SetLootTable(std::string key, int64_t seed) {
            m_lootTable = std::move(key);
            m_lootTableSeed = seed;
        }
        bool               HasLootTable() const { return !m_lootTable.empty(); }
        const std::string& GetLootTable() const { return m_lootTable; }
        int64_t            GetLootTableSeed() const { return m_lootTableSeed; }

        // MC getItem: the rolled find (EMPTY before the first stroke).
        const ItemStack& GetItem() const { return m_item; }
        // MC getHitDirection: -1 while nobody has brushed it.
        int GetHitDirection() const { return m_hitDirection; }

        // MC loadAdditional: the loot table when there is one (the item is
        // then empty), else the item; hit_direction either way.
        void LoadFromNbt(std::string lootTable, int64_t seed, ItemStack item, int hitDirection);

        // ── BlockEntity ───────────────────────────────────────────────────
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        void UnpackLootTable(ILevelWrite& level);
        void BrushingCompleted(ILevelWrite& level);
        void DropContent(ILevelWrite& level);
        int  GetCompletionState() const;
        void SetChanged();
        // level.setBlockAndUpdate(pos, state.setValue(DUSTED, completion)).
        void SetDusted(ILevelWrite& level, int completionState);

        int         m_brushCount = 0;
        int64_t     m_brushCountResetsAtTick = 0;
        int64_t     m_coolDownEndsAtTick = 0;
        ItemStack   m_item{};
        int         m_hitDirection = -1;
        std::string m_lootTable;
        int64_t     m_lootTableSeed = 0;
    };

} // namespace Game
