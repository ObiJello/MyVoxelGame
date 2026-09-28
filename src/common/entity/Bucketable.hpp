// File: src/common/entity/Bucketable.hpp
//
// MC net.minecraft.world.entity.Bucketable — the mobs a water bucket scoops
// up (cod, salmon, pufferfish, tropical fish, axolotl, tadpole). The mob
// classes implement the interface's per-type half (fromBucket,
// saveToBucketTag / loadFromBucketTag, the bucket item, the pickup sound);
// this is the interface's static half, shared by all of them:
//
//   SaveDefaultDataToBucketTag   the CUSTOM_NAME copy and the common
//                                BUCKET_ENTITY_DATA keys (NoAI, Silent,
//                                NoGravity, Glowing, Invulnerable,
//                                PersistenceRequired, Health)
//   LoadDefaultDataFromBucketTag the same keys back onto a released mob
//   BucketMobPickup              the water-bucket click: sound, filled
//                                bucket through createFilledResult, the lead
//                                dropped, the mob discarded
//
// The release half (MobBucketItem.use) is the item behaviour
// Use_MobBucket in ItemBehaviors.cpp.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/entity/Item.hpp"

#include <functional>
#include <optional>

namespace Game {

    class Mob;
    class LivingEntity;
    struct BucketEntityData;
    enum class UseResult : int;

    namespace Bucketable {

        // MC Bucketable.saveDefaultDataToBucketTag.
        void SaveDefaultDataToBucketTag(const Mob& mob, ItemStack& bucket);

        // MC Bucketable.loadDefaultDataFromBucketTag.
        void LoadDefaultDataFromBucketTag(Mob& mob, const BucketEntityData& data);

        // MC Bucketable.bucketMobPickup: nullopt when `held` is not a water
        // bucket (canBePickedUpWithBucket) or the mob is not alive — the
        // caller then falls through to its super.mobInteract. The client
        // answers Success without touching anything (the server does the
        // work and syncs the slot). `saveToBucket` is the mob's
        // saveToBucketTag.
        std::optional<UseResult> BucketMobPickup(Mob& mob, LivingEntity& player, ItemStack& held,
                                                 ItemID bucketItem, const char* pickupSound,
                                                 const std::function<void(ItemStack&)>& saveToBucket);

        // MC Items.*_BUCKET's MobBucketItem(type, WATER, emptySound): the mob
        // a bucket releases and the sound it pours with. nullopt for any
        // other item. (The sulfur cube's bucket has its own behaviour.)
        struct MobBucket {
            EntityTypeId type;
            const char*  emptySound;
        };
        std::optional<MobBucket> MobBucketFor(ItemID item);

    } // namespace Bucketable

} // namespace Game
