// File: src/common/entity/Morph.cpp
#include "common/entity/Morph.hpp"

#include "common/entity/GeneratedItemList.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"

#include <algorithm>
#include <cctype>

namespace Game::Morph {

    bool IsValid(uint32_t code) {
        if (IsNone(code)) return false;
        const uint32_t id = IdOf(code);
        switch (KindOf(code)) {
            case Kind::Mob:   return IsValidEntityType(static_cast<uint16_t>(MobTypeOf(code)));
            case Kind::Item:
                // Block items are 1..BlockID::Count-1; pure items sit at
                // PURE_ITEM_BASE (0x10000) + their table index.
                if (id >= PURE_ITEM_BASE) return id - PURE_ITEM_BASE < kPureItemTableSize;
                return id != Items::Air && id < static_cast<uint32_t>(BlockID::Count);
            case Kind::Block: return BlockOf(code) != 0 && BlockOf(code) < static_cast<uint32_t>(BlockID::Count);
            case Kind::Xp:    return true;
            case Kind::Player: return id == kHerobrine;
        }
        return false;
    }

    bool IsDoubleBlock(uint32_t code) {
        if (!IsValid(code) || KindOf(code) != Kind::Block) return false;
        return BlockStates::Default(static_cast<BlockID>(BlockOf(code))).HasProperty(PropertyId::DOUBLE_BLOCK_HALF);
    }

    BlockState BlockStateOf(uint32_t code, bool* turnedByState, bool upper) {
        if (turnedByState) *turnedByState = false;
        if (!IsValid(code) || KindOf(code) != Kind::Block) return BlockState{};
        BlockState state = BlockStates::Default(static_cast<BlockID>(BlockOf(code)));
        // "half" = upper,lower: index 0 is upper. "open" = true,false: 0 is true.
        if (state.HasProperty(PropertyId::DOUBLE_BLOCK_HALF)) {
            state = state.SetIndex(PropertyId::DOUBLE_BLOCK_HALF, upper ? 0 : 1);
        }
        if (state.HasProperty(PropertyId::OPEN)) {
            state = state.SetIndex(PropertyId::OPEN, IsBlockOpen(code) ? 0 : 1);
        }
        // Quarter turns clockwise from above: north, east, south, west.
        static constexpr Direction kTurns[4] = {
            Direction::North, Direction::East, Direction::South, Direction::West };
        const Direction facing = kTurns[BlockRotationOf(code) & 3u];
        if (state.HasProperty(PropertyId::HORIZONTAL_FACING)) {
            state = state.SetIndex(PropertyId::HORIZONTAL_FACING, HorizontalFacingIndex(facing));
            if (turnedByState) *turnedByState = true;
        } else if (state.HasProperty(PropertyId::FACING)) {
            state = state.SetIndex(PropertyId::FACING, FacingIndex(facing));
            if (turnedByState) *turnedByState = true;
        }
        return state;
    }

    bool IsMob(uint32_t code, EntityTypeId type) {
        return IsValid(code) && KindOf(code) == Kind::Mob &&
               static_cast<EntityTypeId>(MobTypeOf(code)) == type;
    }

    bool ClimbsWalls(uint32_t code) {
        return IsMob(code, EntityTypeId::Spider) || IsMob(code, EntityTypeId::CaveSpider);
    }

    bool IsFlier(uint32_t code) {
        if (!IsValid(code) || KindOf(code) != Kind::Mob) return false;
        switch (static_cast<EntityTypeId>(MobTypeOf(code))) {
            case EntityTypeId::Bat:
            case EntityTypeId::Bee:
            case EntityTypeId::Blaze:
            case EntityTypeId::Parrot:
            case EntityTypeId::Allay:
            case EntityTypeId::Vex:
            case EntityTypeId::Phantom:
            case EntityTypeId::Ghast:
            case EntityTypeId::HappyGhast:
            case EntityTypeId::EnderDragon:
            case EntityTypeId::Wither:
                return true;
            default:
                return false;
        }
    }

    Dims DimsOf(uint32_t code) {
        if (!IsValid(code)) return { 0.6f, 1.8f, 1.62f };
        switch (KindOf(code)) {
            case Kind::Mob: {
                const auto type = static_cast<EntityTypeId>(MobTypeOf(code));
                // A baby morph has the baby's box — the 26.1 BABY_DIMENSIONS
                // where MC declares them, the adult's halved otherwise — the
                // same box a baby mob of the type has (AgeableMob::BaseBb*).
                if (IsBaby(code)) return { GetBabyWidth(type), GetBabyHeight(type), GetEyeHeight(type, true) };
                const EntityTypeInfo& info = GetEntityTypeInfo(type);
                return { info.width, info.height, info.eyeHeight };
            }
            case Kind::Item:  return { 0.25f, 0.25f, 0.25f * 0.85f };   // MC ItemEntity
            case Kind::Block:
                // MC FallingBlockEntity; a two-block-high block (door) is two.
                if (IsDoubleBlock(code)) return { 0.98f, 1.98f, 1.98f * 0.85f };
                return { 0.98f, 0.98f, 0.98f * 0.85f };
            case Kind::Xp:    return { 0.5f,  0.5f,  0.5f  * 0.85f };   // MC ExperienceOrb
            case Kind::Player: return { 0.6f, 1.8f, 1.62f };            // MC Player
        }
        return { 0.6f, 1.8f, 1.62f };
    }

    namespace {
        std::string Normalise(const std::string& text) {
            std::string s;
            for (char c : text) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (s.rfind("minecraft:", 0) == 0) s.erase(0, 10);
            return s;
        }
    }

    bool ParseItemSlug(const std::string& text, ItemID& out) {
        const std::string slug = Normalise(text);
        if (slug.empty()) return false;
        for (size_t i = 0; i < kPureItemTableSize; ++i) {
            if (slug == kPureItemTable[i].slug) {
                out = static_cast<ItemID>(PURE_ITEM_BASE + i);
                return true;
            }
        }
        const BlockState block = BlockStates::FromSlug(slug);
        if (block.Block() != BlockID::Air) {
            out = ItemRegistry::FromBlock(block.Block());
            return true;
        }
        return false;
    }

    std::vector<std::string> ItemSlugs() {
        std::vector<std::string> out;
        out.reserve(kPureItemTableSize + static_cast<size_t>(BlockID::Count));
        for (size_t i = 0; i < kPureItemTableSize; ++i) out.emplace_back(kPureItemTable[i].slug);
        for (int i = 1; i < static_cast<int>(BlockID::Count); ++i) {
            const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
            if (!slug.empty()) out.push_back(slug);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

} // namespace Game::Morph
