// File: src/common/world/block/entity/BannerBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.BannerBlockEntity — what a
// standing or wall banner carries beyond its block: the pattern layers
// (BannerPatternLayers, up to 16 drawn), the custom name, and the item
// components the banner item brought that the block has no field for (MC
// keeps them in BlockEntity.components): the ominous banner's item_name and
// rarity, so breaking an outpost's banner drops the Ominous Banner again.
// The base colour is the block's (white_banner .. black_wall_banner).
//
// Wire (BlockEntityDataS2C — MC getUpdateTag): the pattern layers.
// Disk (BlockEntityNbt.cpp): patterns, CustomName, components.
#pragma once

#include "BlockEntity.hpp"
#include "SignBlockEntity.hpp"   // DyeColor
#include "common/data/DataComponents.hpp"

#include <string>

namespace Game {

    class BannerBlockEntity : public BlockEntity {
    public:
        BannerBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId);

        // MC getBaseColor: the colour the block was registered with.
        DyeColor GetBaseColor() const { return m_baseColor; }
        const BannerPatternLayers& GetPatterns() const { return m_patterns; }
        const std::string& GetCustomName() const { return m_customName; }
        // The components the block keeps for its drop (item_name, rarity).
        const DataComponentMap& GetExtraComponents() const { return m_extra; }

        // MC loadAdditional: the layers and the name; `extra` the leftover
        // item components (the template's / the save's "components").
        void LoadFromNbt(BannerPatternLayers patterns, std::string customName, DataComponentMap extra);

        // ── BlockEntity ───────────────────────────────────────────────────
        // MC applyImplicitComponents / collectImplicitComponents.
        void ApplyItemComponents(const DataComponentMap& components) override;
        void CollectComponents(DataComponentMap& out) const override;
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

        // "red_banner" / "red_wall_banner" → Red; White for anything else.
        static DyeColor BaseColorOf(BlockID block);
        // A wall banner (WallBannerBlock) rather than a standing one.
        static bool IsWallBanner(BlockID block);

    private:
        DyeColor            m_baseColor = DyeColor::White;
        BannerPatternLayers m_patterns;
        std::string         m_customName;
        DataComponentMap    m_extra;
    };

} // namespace Game
