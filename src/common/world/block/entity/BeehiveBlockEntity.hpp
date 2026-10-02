// File: src/common/world/block/entity/BeehiveBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.BeehiveBlockEntity (26.3):
// the up-to-three bees a beehive or bee nest holds — each its saved entity
// data, the ticks it has spent inside and the ticks it must stay (2400 with
// nectar, 600 without) — and the hive's remembered flower.
//
//   * a bee enters (AddOccupant): it is saved (minus the IGNORED_BEE_TAGS)
//     and removed from the level, with the enter sound;
//   * every server tick each occupant counts up; past its minimum it leaves
//     through the hive's front (not while BEES_STAY_IN_HIVE — night or rain —
//     nor with the front blocked), a nectar bee raising the honey level
//     (+1, 1 in 100 +2, capped at 5);
//   * fire next to the hive, a harvest without smoke, an explosion or a
//     break without Silk Touch empties it at once (EMERGENCY), the released
//     bees turning on the player within 4 blocks unless the hive is sedated
//     by a lit campfire up to 5 blocks below;
//   * the BEES item component carries the occupants on a silk-touched nest.
#pragma once

#include "BlockEntity.hpp"
#include "common/data/DataComponents.hpp"

#include <optional>
#include <vector>

namespace Game {

    class Bee;
    class Entity;
    class World;

    class BeehiveBlockEntity : public BlockEntity {
    public:
        static constexpr int kMaxOccupants = 3;
        static constexpr int kMinOccupationTicksNectar = 2400;
        static constexpr int kMinOccupationTicksNectarless = 600;

        enum class ReleaseStatus : uint8_t { HoneyDelivered, BeeReleased, Emergency };

        BeehiveBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        bool IsEmpty() const { return m_stored.empty(); }
        bool IsFull() const { return static_cast<int>(m_stored.size()) >= kMaxOccupants; }
        int  GetOccupantCount() const { return static_cast<int>(m_stored.size()); }

        // MC isFireNearby: fire in the 3x3x3 around the hive.
        bool IsFireNearby(ILevelWrite& level) const;
        // MC isSedated: CampfireBlock.isSmokeyPos.
        bool IsSedated(ILevelWrite& level) const;

        // MC addOccupant: store the bee and take it out of the level.
        void AddOccupant(Bee& bee, ILevelWrite& level);
        // MC emptyAllLivingFromHive: release every occupant that can leave
        // (all of them for EMERGENCY); those within 4 blocks of `player`
        // target it unless the hive is sedated (then they stay out 400 ticks).
        void EmptyAllLivingFromHive(ILevelWrite& level, Entity* player, BlockState state, ReleaseStatus status);
        // MC setChanged's fire check (BeehiveBlock.updateShape and the fire
        // spread call it): fire nearby empties the hive.
        void CheckFire(ILevelWrite& level);

        const std::vector<BeehiveOccupant>& Occupants() const { return m_stored; }
        void SetOccupants(std::vector<BeehiveOccupant> bees) { m_stored = std::move(bees); MarkDirty(); }
        const std::optional<glm::ivec3>& SavedFlowerPos() const { return m_savedFlowerPos; }
        void SetSavedFlowerPos(std::optional<glm::ivec3> pos) { m_savedFlowerPos = pos; }

        // MC applyImplicitComponents / collectImplicitComponents: BEES.
        void ApplyItemComponents(const DataComponentMap& components) override;
        void CollectComponents(DataComponentMap& out) const override;

    private:
        // MC releaseOccupant: false when the bee cannot leave now.
        bool ReleaseOccupant(ILevelWrite& level, BlockState state, const BeehiveOccupant& occupant,
                             std::vector<Entity*>* spawned, ReleaseStatus status);

        std::vector<BeehiveOccupant> m_stored;
        std::optional<glm::ivec3>    m_savedFlowerPos;
    };

    // MC CampfireBlock.isSmokeyPos: a lit campfire up to five blocks below,
    // the smoke stopped by the first block whose collision crosses the
    // centre column (then only a campfire right under it counts).
    bool IsSmokeyPos(const ILevelWrite& level, const glm::ivec3& pos);
    // MC EnvironmentAttributes.BEES_STAY_IN_HIVE at `pos`: night, or rain
    // there (a dimension without weather or a day cycle never keeps them in).
    bool BeesStayInHive(ILevelWrite& level, const glm::ivec3& pos);

    // MC BeehiveBlock.angerNearbyBees: every bee without a target within
    // (8, 6, 8) of the hive turns on a random player in the same box.
    void AngerNearbyBees(ILevelWrite& level, const glm::ivec3& pos);
    // MC BeehiveBlock.getDrops' explosion half: a hive blown up by TNT, a
    // creeper, a wither (or its skull) or a TNT minecart lets its bees out
    // (EMERGENCY) before its loot is rolled. `source` is the blast's direct
    // source entity.
    void BeehiveExplodedBy(ILevelWrite& level, const glm::ivec3& pos, BlockState state, const Entity* source);

} // namespace Game
