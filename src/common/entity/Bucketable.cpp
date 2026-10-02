// File: src/common/entity/Bucketable.cpp
#include "common/entity/Bucketable.hpp"
#include "server/advancements/CriteriaTriggers.hpp"
#include "common/entity/GeneratedItemList.hpp"

#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/sound/SoundEvents.hpp"

namespace Game::Bucketable {

    void SaveDefaultDataToBucketTag(const Mob& mob, ItemStack& bucket) {
        // bucket.copyFrom(CUSTOM_NAME, entity).
        if (const auto& name = mob.GetCustomName()) {
            bucket.components.set(DataComponents::CUSTOM_NAME, *name);
        }
        // CustomData.update(BUCKET_ENTITY_DATA, …): merged into whatever the
        // stack already carries.
        BucketEntityData data = bucket.components.get(DataComponents::BUCKET_ENTITY_DATA)
                                    .value_or(BucketEntityData{});
        if (mob.IsNoAi())                data.noAi = true;
        if (mob.IsSilent())              data.silent = true;
        if (mob.IsNoGravity())           data.noGravity = true;
        // hasGlowingTag: the engine keeps no entity glowing tag (glowing is
        // only the effect), so there is never one to write.
        if (mob.IsInvulnerable())        data.invulnerable = true;
        if (mob.IsPersistenceRequired()) data.persistenceRequired = true;
        data.health = mob.GetHealth();
        bucket.components.set(DataComponents::BUCKET_ENTITY_DATA, data);
    }

    void LoadDefaultDataFromBucketTag(Mob& mob, const BucketEntityData& data) {
        // tag.getBoolean(key).ifPresent(setter) — the keys are only ever
        // written true, so "present" is "true".
        if (data.noAi)         mob.SetNoAi(true);
        if (data.silent)       mob.SetSilent(true);
        if (data.noGravity)    mob.SetNoGravity(true);
        if (data.invulnerable) mob.SetInvulnerable(true);
        if (data.persistenceRequired) mob.SetPersistenceRequired(true);
        if (data.health)       mob.SetHealth(*data.health);
    }

    std::optional<UseResult> BucketMobPickup(Mob& mob, LivingEntity& player, ItemStack& held,
                                             ItemID bucketItem, const char* pickupSound,
                                             const std::function<void(ItemStack&)>& saveToBucket) {
        // canBePickedUpWithBucket: the water bucket, nothing else.
        if (held.itemId != Items::WaterBucket || !mob.IsAlive()) return std::nullopt;
        EntityLevel* level = mob.Level();
        if (!level) return std::nullopt;
        // The client swings and waits for the server's slot update.
        if (level->IsClientSide()) return UseResult::Success;

        // pickupEntity.playSound(getPickupSound(), 1.0F, 1.0F).
        mob.PlaySound(pickupSound, 1.0f, 1.0f);
        ItemStack bucket(bucketItem, 1);
        if (saveToBucket) saveToBucket(bucket);
        // ItemUtils.createFilledResult(itemStack, player, bucket, false).
        level->CreateFilledResult(player, held, bucket);
        // CriteriaTriggers.FILLED_BUCKET with the filled bucket. A leashed
        // mob drops its lead before it goes.
        if (Server::ServerPlayer* sp = Server::CriteriaTriggers::PlayerOf(&player)) {
            Server::CriteriaTriggers::FilledBucket(*sp, bucket);
        }
        mob.DropLeash();
        mob.Discard();
        return UseResult::Success;
    }

    std::optional<MobBucket> MobBucketFor(ItemID item) {
        // Items.java: PUFFERFISH / SALMON / COD / TROPICAL_FISH (EMPTY_FISH),
        // AXOLOTL (EMPTY_AXOLOTL), TADPOLE (EMPTY_TADPOLE).
        if (item == Items::PufferfishBucket)   return MobBucket{EntityTypeId::Pufferfish,   SoundEvents::BUCKET_EMPTY_FISH};
        if (item == Items::SalmonBucket)       return MobBucket{EntityTypeId::Salmon,       SoundEvents::BUCKET_EMPTY_FISH};
        if (item == Items::CodBucket)          return MobBucket{EntityTypeId::Cod,          SoundEvents::BUCKET_EMPTY_FISH};
        if (item == Items::TropicalFishBucket) return MobBucket{EntityTypeId::TropicalFish, SoundEvents::BUCKET_EMPTY_FISH};
        if (item == Items::AxolotlBucket)      return MobBucket{EntityTypeId::Axolotl,      SoundEvents::BUCKET_EMPTY_AXOLOTL};
        if (item == Items::TadpoleBucket)      return MobBucket{EntityTypeId::Tadpole,      SoundEvents::BUCKET_EMPTY_TADPOLE};
        return std::nullopt;
    }

} // namespace Game::Bucketable
