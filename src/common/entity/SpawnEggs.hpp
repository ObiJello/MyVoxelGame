// File: src/common/entity/SpawnEggs.hpp
//
// MC net.minecraft.world.item.SpawnEggItem — which entity a spawn egg makes.
//
// In MC the link is a DataComponents.ENTITY_DATA component carried by the item,
// registered from Items.java (`registerItem("zombie_spawn_egg", SpawnEggItem::new,
// new Item.Properties().component(ENTITY_DATA, TypedEntityData.of(EntityType.ZOMBIE)))`).
// This port has no component that can hold an EntityType, so the mapping is an
// explicit table instead.
//
// One row per mob that has an egg — GENERATED from the entity table by hand of
// tools/gen_entity_types.py's output, kept here rather than in a fourth
// generator because the only thing it adds is the ItemID name.
//
// This is EVERY *_spawn_egg item in GeneratedItemList — all 87. The only two
// implemented mobs without a row are giant and illusioner, which have no spawn
// egg in MC either.
//
// If you add a row, make sure the entity type has a MobDef (or a case in
// IntegratedServer::MakeMob). An egg naming a type with neither is inert: the
// use path succeeds, MakeGenericMob returns null, and nothing spawns.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"

namespace Game {

    struct SpawnEggEntry {
        ItemID       item;
        EntityTypeId type;
    };

    inline constexpr SpawnEggEntry kSpawnEggTable[] = {
        { Items::ZombieSpawnEgg, EntityTypeId::Zombie },
        { Items::SkeletonSpawnEgg, EntityTypeId::Skeleton },
        { Items::CreeperSpawnEgg, EntityTypeId::Creeper },
        { Items::SpiderSpawnEgg, EntityTypeId::Spider },
        { Items::CowSpawnEgg, EntityTypeId::Cow },
        { Items::PigSpawnEgg, EntityTypeId::Pig },
        { Items::SheepSpawnEgg, EntityTypeId::Sheep },
        { Items::ChickenSpawnEgg, EntityTypeId::Chicken },
        { Items::AllaySpawnEgg, EntityTypeId::Allay },
        { Items::ArmadilloSpawnEgg, EntityTypeId::Armadillo },
        { Items::AxolotlSpawnEgg, EntityTypeId::Axolotl },
        { Items::BatSpawnEgg, EntityTypeId::Bat },
        { Items::BeeSpawnEgg, EntityTypeId::Bee },
        { Items::BlazeSpawnEgg, EntityTypeId::Blaze },
        { Items::BoggedSpawnEgg, EntityTypeId::Bogged },
        { Items::BreezeSpawnEgg, EntityTypeId::Breeze },
        { Items::CamelSpawnEgg, EntityTypeId::Camel },
        { Items::CamelHuskSpawnEgg, EntityTypeId::CamelHusk },
        { Items::CatSpawnEgg, EntityTypeId::Cat },
        { Items::CaveSpiderSpawnEgg, EntityTypeId::CaveSpider },
        { Items::CodSpawnEgg, EntityTypeId::Cod },
        { Items::CopperGolemSpawnEgg, EntityTypeId::CopperGolem },
        { Items::CreakingSpawnEgg, EntityTypeId::Creaking },
        { Items::DolphinSpawnEgg, EntityTypeId::Dolphin },
        { Items::DonkeySpawnEgg, EntityTypeId::Donkey },
        { Items::DrownedSpawnEgg, EntityTypeId::Drowned },
        { Items::ElderGuardianSpawnEgg, EntityTypeId::ElderGuardian },
        { Items::EnderDragonSpawnEgg, EntityTypeId::EnderDragon },
        { Items::EndermanSpawnEgg, EntityTypeId::Enderman },
        { Items::EndermiteSpawnEgg, EntityTypeId::Endermite },
        { Items::EvokerSpawnEgg, EntityTypeId::Evoker },
        { Items::FoxSpawnEgg, EntityTypeId::Fox },
        { Items::FrogSpawnEgg, EntityTypeId::Frog },
        { Items::GhastSpawnEgg, EntityTypeId::Ghast },
        { Items::GlowSquidSpawnEgg, EntityTypeId::GlowSquid },
        { Items::GoatSpawnEgg, EntityTypeId::Goat },
        { Items::GuardianSpawnEgg, EntityTypeId::Guardian },
        { Items::HappyGhastSpawnEgg, EntityTypeId::HappyGhast },
        { Items::HoglinSpawnEgg, EntityTypeId::Hoglin },
        { Items::HorseSpawnEgg, EntityTypeId::Horse },
        { Items::HuskSpawnEgg, EntityTypeId::Husk },
        { Items::IronGolemSpawnEgg, EntityTypeId::IronGolem },
        { Items::LlamaSpawnEgg, EntityTypeId::Llama },
        { Items::MagmaCubeSpawnEgg, EntityTypeId::MagmaCube },
        { Items::MooshroomSpawnEgg, EntityTypeId::Mooshroom },
        { Items::MuleSpawnEgg, EntityTypeId::Mule },
        { Items::NautilusSpawnEgg, EntityTypeId::Nautilus },
        { Items::OcelotSpawnEgg, EntityTypeId::Ocelot },
        { Items::PandaSpawnEgg, EntityTypeId::Panda },
        { Items::ParchedSpawnEgg, EntityTypeId::Parched },
        { Items::ParrotSpawnEgg, EntityTypeId::Parrot },
        { Items::PhantomSpawnEgg, EntityTypeId::Phantom },
        { Items::PiglinSpawnEgg, EntityTypeId::Piglin },
        { Items::PiglinBruteSpawnEgg, EntityTypeId::PiglinBrute },
        { Items::PillagerSpawnEgg, EntityTypeId::Pillager },
        { Items::PolarBearSpawnEgg, EntityTypeId::PolarBear },
        { Items::PufferfishSpawnEgg, EntityTypeId::Pufferfish },
        { Items::RabbitSpawnEgg, EntityTypeId::Rabbit },
        { Items::RavagerSpawnEgg, EntityTypeId::Ravager },
        { Items::SalmonSpawnEgg, EntityTypeId::Salmon },
        { Items::ShulkerSpawnEgg, EntityTypeId::Shulker },
        { Items::SilverfishSpawnEgg, EntityTypeId::Silverfish },
        { Items::SkeletonHorseSpawnEgg, EntityTypeId::SkeletonHorse },
        { Items::SlimeSpawnEgg, EntityTypeId::Slime },
        { Items::SnifferSpawnEgg, EntityTypeId::Sniffer },
        { Items::SnowGolemSpawnEgg, EntityTypeId::SnowGolem },
        { Items::SquidSpawnEgg, EntityTypeId::Squid },
        { Items::StraySpawnEgg, EntityTypeId::Stray },
        { Items::StriderSpawnEgg, EntityTypeId::Strider },
        { Items::TadpoleSpawnEgg, EntityTypeId::Tadpole },
        { Items::TraderLlamaSpawnEgg, EntityTypeId::TraderLlama },
        { Items::TropicalFishSpawnEgg, EntityTypeId::TropicalFish },
        { Items::TurtleSpawnEgg, EntityTypeId::Turtle },
        { Items::VexSpawnEgg, EntityTypeId::Vex },
        { Items::VindicatorSpawnEgg, EntityTypeId::Vindicator },
        { Items::VillagerSpawnEgg, EntityTypeId::Villager },
        { Items::WanderingTraderSpawnEgg, EntityTypeId::WanderingTrader },
        { Items::WardenSpawnEgg, EntityTypeId::Warden },
        { Items::WitchSpawnEgg, EntityTypeId::Witch },
        { Items::WitherSpawnEgg, EntityTypeId::Wither },
        { Items::WitherSkeletonSpawnEgg, EntityTypeId::WitherSkeleton },
        { Items::WolfSpawnEgg, EntityTypeId::Wolf },
        { Items::ZoglinSpawnEgg, EntityTypeId::Zoglin },
        { Items::ZombieHorseSpawnEgg, EntityTypeId::ZombieHorse },
        { Items::ZombieNautilusSpawnEgg, EntityTypeId::ZombieNautilus },
        { Items::ZombieVillagerSpawnEgg, EntityTypeId::ZombieVillager },
        { Items::ZombifiedPiglinSpawnEgg, EntityTypeId::ZombifiedPiglin },
    };

    // MC SpawnEggItem.getType(stack). Returns EntityTypeId::Count when the item
    // is not a spawn egg this port implements — the equivalent of MC's null,
    // which its useOn turns into InteractionResult.FAIL.
    inline EntityTypeId SpawnEggEntityType(ItemID item) {
        for (const SpawnEggEntry& e : kSpawnEggTable) {
            if (e.item == item) return e.type;
        }
        return EntityTypeId::Count;
    }

} // namespace Game
