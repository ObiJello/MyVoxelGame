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

    bool HasMobSize(EntityTypeId type) {
        switch (type) {
            case EntityTypeId::Slime:
            case EntityTypeId::MagmaCube:
            case EntityTypeId::MazeSlime:
            case EntityTypeId::Phantom:
            case EntityTypeId::Pufferfish:
                return true;
            default:
                return false;
        }
    }

    int MobSizeOf(uint32_t code) {
        if (!IsValid(code) || KindOf(code) != Kind::Mob) return 0;
        const int bits = static_cast<int>(MobSizeBitsOf(code));
        switch (static_cast<EntityTypeId>(MobTypeOf(code))) {
            case EntityTypeId::Slime:
            case EntityTypeId::MagmaCube:
            case EntityTypeId::MazeSlime:  return std::max(1, bits);   // Slime.setSize clamps to 1..
            case EntityTypeId::Phantom:    return bits;
            case EntityTypeId::Pufferfish: return std::min(bits, 2);   // STATE_SMALL..STATE_FULL
            default:                       return 0;
        }
    }

    namespace {
        EntityTypeId MobTypeOrNone(uint32_t code, bool& isMob) {
            isMob = IsValid(code) && KindOf(code) == Kind::Mob;
            return isMob ? static_cast<EntityTypeId>(MobTypeOf(code)) : EntityTypeId::Zombie;
        }
    }

    bool IsWaterBound(uint32_t code) {
        bool isMob = false;
        const EntityTypeId type = MobTypeOrNone(code, isMob);
        if (!isMob) return false;
        switch (type) {
            case EntityTypeId::Cod:
            case EntityTypeId::Salmon:
            case EntityTypeId::TropicalFish:
            case EntityTypeId::Pufferfish:
            case EntityTypeId::Tadpole:
            case EntityTypeId::Dolphin:
            case EntityTypeId::Guardian:
            case EntityTypeId::ElderGuardian:
            case EntityTypeId::Squid:
            case EntityTypeId::GlowSquid:
            case EntityTypeId::Nautilus:
            case EntityTypeId::ZombieNautilus:
                return true;
            default:
                return false;
        }
    }

    Flop FlopOf(uint32_t code) {
        bool isMob = false;
        const EntityTypeId type = MobTypeOrNone(code, isMob);
        if (!isMob) return { 0.0f, 0.0f };
        switch (type) {
            // AbstractFish.aiStep: (nextFloat * 2 - 1) * 0.05 sideways, 0.4 up.
            case EntityTypeId::Cod:
            case EntityTypeId::Salmon:
            case EntityTypeId::TropicalFish:
            case EntityTypeId::Pufferfish:
            case EntityTypeId::Tadpole:
                return { 0.05f, 0.4f };
            // Dolphin.tick: (nextFloat * 2 - 1) * 0.2 sideways, 0.5 up.
            case EntityTypeId::Dolphin:
                return { 0.2f, 0.5f };
            // Guardian.aiStep: (nextFloat * 2 - 1) * 0.4 sideways, 0.5 up.
            case EntityTypeId::Guardian:
            case EntityTypeId::ElderGuardian:
                return { 0.4f, 0.5f };
            default:
                return { 0.0f, 0.0f };
        }
    }

    bool IsStationary(uint32_t code) {
        return IsMob(code, EntityTypeId::Shulker);
    }

    float SwimAccelOf(uint32_t code) {
        bool isMob = false;
        const EntityTypeId type = MobTypeOrNone(code, isMob);
        if (!isMob) return 0.0f;
        // moveRelative(speed, input) adds speed × the input (normalised when
        // longer than 1) a tick; the input's length is the mob's zza, which
        // its move control sets to MOVEMENT_SPEED × the pace's modifier.
        switch (type) {
            // AbstractFish.travelInWater moveRelative(0.01) × FishMoveControl
            // speed 0.7 (MOVEMENT_SPEED 0.7 × FishSwimGoal's 1.0).
            case EntityTypeId::Cod:
            case EntityTypeId::Salmon:
            case EntityTypeId::TropicalFish:
            case EntityTypeId::Pufferfish:
                return 0.01f * 0.7f;
            // The same 0.01 stroke; TadpoleAi's RandomStroll.swim(0.5) on its
            // MOVEMENT_SPEED 1.0 makes zza 0.5.
            case EntityTypeId::Tadpole:
                return 0.01f * 0.5f;
            // Dolphin.travelInWater moveRelative(getSpeed()): 1.2 ×
            // RandomSwimmingGoal 1.0 × SmoothSwimmingMoveControl's 0.02, the
            // input (zza 1.2) normalised to 1.
            case EntityTypeId::Dolphin:
                return 1.2f * 0.02f;
            // Guardian.travelInWater moveRelative(0.1) × GuardianMoveControl
            // speed (MOVEMENT_SPEED 0.5 / 0.3 × RandomStrollGoal 1.0).
            case EntityTypeId::Guardian:
                return 0.1f * 0.5f;
            case EntityTypeId::ElderGuardian:
                return 0.1f * 0.3f;
            // Turtle.travelInWater moveRelative(0.1) × TurtleMoveControl speed
            // (MOVEMENT_SPEED 0.25 × TurtleRandomStrollGoal 1.0).
            case EntityTypeId::Turtle:
                return 0.1f * 0.25f;
            // Axolotl.travelInWater moveRelative(getSpeed()): MOVEMENT_SPEED
            // 1.0 × AxolotlAi's RandomStroll.swim(0.5) × the control's 0.1 in
            // water, times zza 0.5.
            case EntityTypeId::Axolotl:
                return (1.0f * 0.5f * 0.1f) * 0.5f;
            // Frog.travelInWater moveRelative(getSpeed()): 1.0 × FrogAi's
            // swim 0.75 × the control's 0.02, times zza 0.75.
            case EntityTypeId::Frog:
                return (1.0f * 0.75f * 0.02f) * 0.75f;
            // Squid.aiStep pushes its own movement vector (up to 0.2 a tick
            // at the stroke's peak, coasting at 0.9 between): the mean of
            // that stroke as a steady push.
            case EntityTypeId::Squid:
            case EntityTypeId::GlowSquid:
                return 0.01f;
            // AbstractNautilus.travelInWater moveRelative(getSpeed()):
            // MOVEMENT_SPEED (1.0 / 1.1) × SmoothSwimmingMoveControl's 0.02.
            case EntityTypeId::Nautilus:
                return 1.0f * 0.02f;
            case EntityTypeId::ZombieNautilus:
                return 1.1f * 0.02f;
            default:
                return 0.0f;
        }
    }

    int32_t DefaultVariantOf(uint32_t code) {
        if (IsNone(code) || KindOf(code) != Kind::Mob) return 0;
        // Salmon.Variant.DEFAULT = MEDIUM; TropicalFish.DEFAULT_VARIANT packs
        // to 0 (KOB, white, white); Parrot.Variant.DEFAULT = RED_BLUE (0).
        return static_cast<EntityTypeId>(MobTypeOf(code)) == EntityTypeId::Salmon ? 1 : 0;
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

    float FlightSpeedFactorOf(uint32_t code) {
        if (!IsFlier(code)) return 1.0f;
        constexpr float kPlayerFlight = 0.05f * 0.98f;   // Abilities.flyingSpeed × the input
        switch (static_cast<EntityTypeId>(MobTypeOf(code))) {
            case EntityTypeId::Bee:    return 0.02f * 0.6f / kPlayerFlight;   // FLYING_SPEED 0.6
            case EntityTypeId::Parrot: return 0.02f * 0.4f / kPlayerFlight;   // FLYING_SPEED 0.4
            // Allay.travel: moveRelative(getSpeed()) with the input's zza the
            // same speed — FLYING_SPEED 0.1, squared.
            case EntityTypeId::Allay:  return 0.1f * 0.1f / kPlayerFlight;
            default:                   return 1.0f;
        }
    }

    Dims DimsOf(uint32_t code) {
        if (!IsValid(code)) return { 0.6f, 1.8f, 1.62f };
        switch (KindOf(code)) {
            case Kind::Mob: {
                const auto type = static_cast<EntityTypeId>(MobTypeOf(code));
                const EntityTypeInfo& info = GetEntityTypeInfo(type);
                // The sized bodies: MC getDefaultDimensions scales the type's
                // whole EntityDimensions (eye height included) — by the size
                // (AbstractCubeMob), 1 + 0.15 × size (Phantom), the puff
                // state's 0.5 / 0.7 / 1.0 (Pufferfish). A sulfur cube is
                // size 1 as a baby and 2 grown (SulfurCube.setSpawnSize), and
                // its box ignores the baby scale (AbstractCubeMob's override).
                float sizeScale = 0.0f;
                switch (type) {
                    case EntityTypeId::Slime:
                    case EntityTypeId::MagmaCube:
                    case EntityTypeId::MazeSlime:
                        sizeScale = static_cast<float>(MobSizeOf(code));
                        break;
                    case EntityTypeId::Phantom:
                        sizeScale = 1.0f + 0.15f * static_cast<float>(MobSizeOf(code));
                        break;
                    case EntityTypeId::Pufferfish: {
                        static constexpr float kPuffScale[3] = { 0.5f, 0.7f, 1.0f };
                        sizeScale = kPuffScale[MobSizeOf(code)];
                        break;
                    }
                    case EntityTypeId::SulfurCube:
                        sizeScale = IsBaby(code) ? 1.0f : 2.0f;
                        break;
                    default:
                        break;
                }
                if (sizeScale > 0.0f) {
                    return { info.width * sizeScale, info.height * sizeScale, info.eyeHeight * sizeScale };
                }
                // A baby morph has the baby's box — the 26.1 BABY_DIMENSIONS
                // where MC declares them, the adult's halved otherwise — the
                // same box a baby mob of the type has (AgeableMob::BaseBb*).
                if (IsBaby(code)) return { GetBabyWidth(type), GetBabyHeight(type), GetEyeHeight(type, true) };
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
