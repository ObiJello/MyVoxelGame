// File: src/server/advancements/CriteriaTriggers.hpp
//
// MC CriteriaTriggers: one entry point per trigger, called from the gameplay
// site MC calls it from. Each looks up the player's listeners for that
// trigger, tests every listening criterion's conditions (its own fields, then
// the shared `player` predicate — SimpleCriterionTrigger.trigger) and awards
// the criteria that matched.
//
// Light on purpose: shared (common) code includes this header at its hook
// points, so it names engine types by forward declaration only. A call made
// on the client — prediction runs the same item and entity code — resolves no
// server player (PlayerOf answers null) and does nothing.
//
// Nothing here awards while advancements are switched off for the world
// (cheats on, advancements_with_cheats off — ServerAdvancements.hpp).
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game {
    class Entity;
    class IUsePlayer;
    struct ItemStack;
    struct DamageSourceInfo;
}

namespace Server { class ServerPlayer; }

namespace Server::Advancements {

    // MC CriteriaTriggers' registry, in its order.
    enum class TriggerType : uint8_t {
        Impossible, PlayerKilledEntity, EntityKilledPlayer, EnterBlock, InventoryChanged,
        RecipeUnlocked, PlayerHurtEntity, EntityHurtPlayer, EnchantedItem, FilledBucket,
        BrewedPotion, ConstructBeacon, UsedEnderEye, SummonedEntity, BredAnimals,
        Location, SleptInBed, CuredZombieVillager, VillagerTrade, ItemDurabilityChanged,
        Levitation, ChangedDimension, Tick, TameAnimal, PlacedBlock,
        ConsumeItem, EffectsChanged, UsedTotem, NetherTravel, FishingRodHooked,
        ChanneledLightning, ShotCrossbow, SpearMobs, KilledByArrow, HeroOfTheVillage,
        VoluntaryExile, SlideDownBlock, BeeNestDestroyed, TargetHit, ItemUsedOnBlock,
        DefaultBlockUse, AnyBlockUse, PlayerGeneratesContainerLoot, ThrownItemPickedUpByEntity,
        ThrownItemPickedUpByPlayer, PlayerInteractedWithEntity, PlayerShearedEquipment,
        StartedRiding, LightningStrike, UsingItem, FallFromHeight, RideEntityInLava,
        KillMobNearSculkCatalyst, AllayDropItemOnBlock, AvoidVibration, RecipeCrafted,
        CrafterRecipeCrafted, FallAfterExplosion,
        Count
    };

    // "minecraft:inventory_changed" → InventoryChanged; nullopt if unknown.
    std::optional<TriggerType> TriggerByName(std::string_view id);
    const char* TriggerName(TriggerType type);

} // namespace Server::Advancements

namespace Server::CriteriaTriggers {

    // The server player behind a level's player entity (PlayerEntityView) or
    // an item-use player; null on the client and for anything else.
    ServerPlayer* PlayerOf(const Game::Entity* entity);
    ServerPlayer* PlayerOf(Game::IUsePlayer* user);

    // ── Kills and damage ─────────────────────────────────────────────────
    void PlayerKilledEntity(ServerPlayer& player, Game::Entity& victim, const Game::DamageSourceInfo& source);
    void EntityKilledPlayer(ServerPlayer& player, Game::Entity& killer, const Game::DamageSourceInfo& source);
    void KillMobNearSculkCatalyst(ServerPlayer& player, Game::Entity& victim, const Game::DamageSourceInfo& source);
    void KilledByArrow(ServerPlayer& player, const std::vector<Game::Entity*>& victims,
                       const Game::ItemStack* firedFromWeapon);
    void PlayerHurtEntity(ServerPlayer& player, Game::Entity& victim, const Game::DamageSourceInfo& source,
                          float dealt, float taken, bool blocked);
    void EntityHurtPlayer(ServerPlayer& player, const Game::DamageSourceInfo& source,
                          float dealt, float taken, bool blocked);
    void UsedTotem(ServerPlayer& player, const Game::ItemStack& totem);
    void ChanneledLightning(ServerPlayer& player, const std::vector<Game::Entity*>& victims);
    void LightningStrike(ServerPlayer& player, Game::Entity& bolt, const std::vector<Game::Entity*>& bystanders);
    void TargetHit(ServerPlayer& player, Game::Entity& projectile, const glm::dvec3& hitPos, int signalStrength);
    void SpearMobs(ServerPlayer& player, int count);
    void ShotCrossbow(ServerPlayer& player, const Game::ItemStack& crossbow);

    // ── Plain player triggers (PlayerTrigger) ────────────────────────────
    void SleptInBed(ServerPlayer& player);
    void AvoidVibration(ServerPlayer& player);
    void HeroOfTheVillage(ServerPlayer& player);   // RAID_WIN
    void VoluntaryExile(ServerPlayer& player);     // RAID_OMEN
    void StartedRiding(ServerPlayer& player);

    // ── Items ────────────────────────────────────────────────────────────
    void InventoryChanged(ServerPlayer& player, const Game::ItemStack& changed);
    void ConsumeItem(ServerPlayer& player, const Game::ItemStack& item);
    void FilledBucket(ServerPlayer& player, const Game::ItemStack& bucket);
    void UsingItem(ServerPlayer& player, const Game::ItemStack& item);
    void EnchantedItem(ServerPlayer& player, const Game::ItemStack& item, int levels);
    // `potion` is the brewed PotionContents' potion id ("minecraft:swiftness"),
    // empty for a custom-only contents.
    void BrewedPotion(ServerPlayer& player, const std::string& potion);
    void VillagerTrade(ServerPlayer& player, Game::Entity& villager, const Game::ItemStack& result);
    void RecipeUnlocked(ServerPlayer& player, const std::string& recipeId);
    void RecipeCrafted(ServerPlayer& player, const std::string& recipeId, const std::vector<Game::ItemStack>& ingredients);
    void CrafterRecipeCrafted(ServerPlayer& player, const std::string& recipeId,
                              const std::vector<Game::ItemStack>& ingredients);
    void FishingRodHooked(ServerPlayer& player, const Game::ItemStack& rod, Game::Entity& hook,
                          const std::vector<Game::ItemStack>& items);
    void ItemDurabilityChanged(ServerPlayer& player, const Game::ItemStack& item, int newDurability);
    void UsedEnderEye(ServerPlayer& player, const glm::ivec3& feature);
    void GenerateContainerLoot(ServerPlayer& player, const std::string& lootTable);

    // ── Entities ─────────────────────────────────────────────────────────
    void PlayerInteractedWithEntity(ServerPlayer& player, const Game::ItemStack& item, Game::Entity& target);
    void PlayerShearedEquipment(ServerPlayer& player, const Game::ItemStack& item, Game::Entity& target);
    void ThrownItemPickedUpByPlayer(ServerPlayer& player, const Game::ItemStack& item, Game::Entity* thrower);
    void ThrownItemPickedUpByEntity(ServerPlayer& thrower, const Game::ItemStack& item, Game::Entity& pickedUpBy);
    void BredAnimals(ServerPlayer& player, Game::Entity& parent, Game::Entity& partner, Game::Entity* child);
    void TameAnimal(ServerPlayer& player, Game::Entity& animal);
    void SummonedEntity(ServerPlayer& player, Game::Entity& entity);
    void CuredZombieVillager(ServerPlayer& player, Game::Entity& zombie, Game::Entity& villager);

    // ── Blocks ───────────────────────────────────────────────────────────
    void PlacedBlock(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& tool);
    void ItemUsedOnBlock(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& tool);
    void AllayDropItemOnBlock(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& item);
    void DefaultBlockUse(ServerPlayer& player, const glm::ivec3& pos);
    void AnyBlockUse(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& tool);
    void BeeNestDestroyed(ServerPlayer& player, Game::BlockState state, const Game::ItemStack& tool, int beesInside);
    void ConstructBeacon(ServerPlayer& player, int levels);
    void SlideDownBlock(ServerPlayer& player, Game::BlockState state);
    void EnterBlock(ServerPlayer& player, Game::BlockState state);

    // ── Player state ─────────────────────────────────────────────────────
    // MC onEffectAdded / onEffectUpdated (source may be null) and
    // onEffectsRemoved (no source). `levitation` marks a LEVITATION
    // added (true) or removed (false) — the levitation trigger's start.
    void EffectsChanged(ServerPlayer& player, Game::Entity* source);
    void LevitationStarted(ServerPlayer& player);
    void LevitationStopped(ServerPlayer& player);
    // MC ServerPlayer.teleport(TeleportTransition) — CHANGED_DIMENSION, and
    // NETHER_TRAVEL when coming home from the Nether. `positionBefore` is
    // where the player stood in the old dimension.
    void ChangedDimension(ServerPlayer& player, Game::DimensionId from, Game::DimensionId to,
                          const glm::dvec3& positionBefore);
    // MC Player.currentImpulseImpactPos / currentExplosionCause: an explosion
    // (a wind charge) launched the player; FALL_AFTER_EXPLOSION reads it when
    // the fall begins.
    void ExplosionLaunched(ServerPlayer& player, const glm::dvec3& impactPos, Game::Entity* cause);
    // The fall / levitation / lava-ride / location / inventory triggers MC
    // drives from ServerPlayer.doTick run from ServerAdvancements::TickPlayer.

    // ── Player-tick triggers (called by ServerAdvancements::TickPlayer) ──
    void Tick(ServerPlayer& player);
    void Location(ServerPlayer& player);
    void Levitation(ServerPlayer& player, const glm::dvec3& start, int duration);
    void FallFromHeight(ServerPlayer& player, const glm::dvec3& start);
    void FallAfterExplosion(ServerPlayer& player, const glm::dvec3& start, Game::Entity* cause);
    void RideEntityInLava(ServerPlayer& player, const glm::dvec3& start);
    void NetherTravel(ServerPlayer& player, const glm::dvec3& entered);

} // namespace Server::CriteriaTriggers
