// File: src/common/world/block/entity/BannerBlockEntity.cpp
#include "BannerBlockEntity.hpp"

#include "common/network/PacketRegistry.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <string_view>

namespace Game {

    DyeColor BannerBlockEntity::BaseColorOf(BlockID block) {
        std::string_view slug = BlockRegistry::Get(block).registrySlug;
        constexpr std::string_view kWall = "_wall_banner";
        constexpr std::string_view kStanding = "_banner";
        if (slug.size() > kWall.size() && slug.substr(slug.size() - kWall.size()) == kWall) {
            slug = slug.substr(0, slug.size() - kWall.size());
        } else if (slug.size() > kStanding.size() && slug.substr(slug.size() - kStanding.size()) == kStanding) {
            slug = slug.substr(0, slug.size() - kStanding.size());
        }
        DyeColor color = DyeColor::White;
        DyeColorFromName(slug, color);
        return color;
    }

    bool BannerBlockEntity::IsWallBanner(BlockID block) {
        return BlockRegistry::Get(block).registrySlug.find("_wall_banner") != std::string::npos;
    }

    BannerBlockEntity::BannerBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
        : BlockEntity(type, worldPos, blockId), m_baseColor(BaseColorOf(blockId)) {}

    void BannerBlockEntity::LoadFromNbt(BannerPatternLayers patterns, std::string customName,
                                        DataComponentMap extra) {
        m_patterns = std::move(patterns);
        m_customName = std::move(customName);
        m_extra = std::move(extra);
    }

    void BannerBlockEntity::ApplyItemComponents(const DataComponentMap& components) {
        // BannerBlockEntity.applyImplicitComponents: BANNER_PATTERNS and
        // CUSTOM_NAME; what the block has no field for stays in its
        // components (the ominous banner's item_name and rarity).
        if (auto patterns = components.get(DataComponents::BANNER_PATTERNS)) m_patterns = *patterns;
        if (auto name = components.get(DataComponents::CUSTOM_NAME)) m_customName = *name;
        for (const char* key : { "item_name", "rarity" }) m_extra.CopyNamed(components, key);
        MarkDirty();
    }

    void BannerBlockEntity::CollectComponents(DataComponentMap& out) const {
        for (const char* key : { "item_name", "rarity" }) out.CopyNamed(m_extra, key);
        if (!m_patterns.IsEmpty()) out.set(DataComponents::BANNER_PATTERNS, m_patterns);
        if (!m_customName.empty()) out.set(DataComponents::CUSTOM_NAME, m_customName);
    }

    void BannerBlockEntity::Save(Network::PacketBuffer& out) const {
        out.WriteVarInt(static_cast<uint32_t>(m_patterns.layers.size()));
        for (const BannerPatternLayer& layer : m_patterns.layers) {
            out.WriteString(layer.pattern);
            out.WriteVarInt(layer.color);
        }
    }

    void BannerBlockEntity::Load(Network::PacketReader& in) {
        m_patterns.layers.clear();
        if (!in.HasMore()) return;
        const uint32_t count = std::min<uint32_t>(in.ReadVarInt(), 64);
        for (uint32_t i = 0; i < count && in.HasMore(); ++i) {
            BannerPatternLayer layer;
            layer.pattern = in.ReadString();
            layer.color = static_cast<uint8_t>(in.ReadVarInt() & 15);
            m_patterns.layers.push_back(std::move(layer));
        }
    }

} // namespace Game
