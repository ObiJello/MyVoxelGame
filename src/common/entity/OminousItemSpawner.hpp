// File: src/common/entity/OminousItemSpawner.hpp
//
// MC net.minecraft.world.entity.OminousItemSpawner — the swirling ominous
// item an ominous trial spawner hangs over a player (or one of its mobs)
// every 160 ticks while its wave is running. After 60..120 ticks it lets the
// item go: a projectile item (arrows, tipped arrows, splash / lingering
// potions, fire charges, wind charges, snowballs, eggs, tridents) is FIRED
// straight down through its ProjectileItem dispense config, anything else
// drops as an item entity. 36 ticks before that it plays the "about to spawn
// item" cue; every 5 ticks the client pulls OMINOUS_SPAWNING motes into it.
//
// Architecture: a plain Entity in MC; here it rides the projectile Misc
// pipeline like the lightning bolt and the evoker fangs (Projectile.hpp): no
// gravity, no physics, not pickable, not attackable, never pushed. The item
// (MC's synched DATA_ITEM) reaches the client through ItemFrameDataS2C — the
// engine's "an entity's displayed item" packet — and is drawn by
// ItemEntityRenderer (MC OminousItemSpawnerRenderer: grows over its first 50
// ticks, spins 40 degrees a tick, full bright).
#pragma once

#include "common/entity/Item.hpp"
#include "common/entity/projectile/Projectile.hpp"

#include <cstdint>
#include <memory>

namespace Game {

    class OminousItemSpawner : public Projectile {
    public:
        static constexpr int kSpawnItemDelayMin = 60;
        static constexpr int kSpawnItemDelayMax = 120;
        static constexpr int kTicksBeforeAboutToSpawnSound = 36;

        explicit OminousItemSpawner(EntityLevel* level);

        // MC OminousItemSpawner.create(level, item): the delay drawn from
        // the level's random, the item set. The caller positions it.
        static std::unique_ptr<OminousItemSpawner> Create(EntityLevel& level, const ItemStack& item);

        void Tick() override;

        // MC DATA_ITEM.
        const ItemStack& GetItem() const { return m_item; }
        void SetItem(const ItemStack& item);
        // The tracker's send-on-change latch (ItemFrameDataS2C). Read-and-clear.
        bool ConsumeItemDirty() {
            const bool was = m_itemDirty;
            m_itemDirty = false;
            return was;
        }

        // Save/load: MC "spawn_item_after_ticks" and "item".
        int64_t GetSpawnItemAfterTicks() const { return m_spawnItemAfterTicks; }
        void SetSpawnItemAfterTicks(int64_t ticks) { m_spawnItemAfterTicks = ticks; }

        // MC: noPhysics; hurtServer false; not pickable (Entity default is
        // false in MC — the engine's default is true); no passengers;
        // PushReaction.IGNORE_ENTITY.
        bool Hurt(MobDamageSource, float, Entity*) override { return false; }
        bool IsPickable() const override { return false; }
        bool IsAttackable() const override { return false; }
        bool IsPushable() const override { return false; }
        bool IsPushedByFluid() const override { return false; }
        bool CanAddPassenger(const Entity&) const override { return false; }
        bool CouldAcceptPassenger() const override { return false; }
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason, std::shared_ptr<SpawnGroupData> groupData) override {
            return groupData;
        }

    private:
        void TickServer();
        void TickClient();
        // MC spawnItem / spawnProjectile.
        void SpawnItem();

        ItemStack m_item;
        bool      m_itemDirty = false;
        int64_t   m_spawnItemAfterTicks = 0;
    };

} // namespace Game
