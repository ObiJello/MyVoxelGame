#pragma once
#include <vector>
class Random;
namespace console {
// Survival block breaking, transferred from Tile::getDestroyProgress,
// Player::getDestroySpeed/canDestroy, Inventory::getDestroySpeed/canDestroy and
// the DiggerItem, PickaxeItem, ShovelItem, HatchetItem, WeaponItem and
// ShearsItem overrides. Item IDs are the registered IDs (shift already added).
struct SurvivalDrop { int id=0,count=1,damage=0; };

// Item::getDestroySpeed for the held item (1 for anything that is not a tool).
float consoleItemDestroySpeed(int itemId,int tileId);
// Item::canDestroySpecial for the held item.
bool consoleItemCanDestroySpecial(int itemId,int tileId);
// Inventory::canDestroy: the material is always destroyable, or the held item can.
bool consolePlayerCanDestroy(int tileId,int heldItemId);
// Player::getDestroySpeed. Effect amplifiers are -1 when the effect is absent.
// The console removed Java's airborne /5 penalty (see Player.cpp comment).
float consolePlayerDestroySpeed(int tileId,int heldItemId,bool underWater,
                                int digSpeedAmplifier=-1,int digSlowAmplifier=-1);
// Tile::getDestroyProgress: per-tick progress, 0 for indestructible tiles and
// >=1 for instant breaks. Unregistered tiles cannot be destroyed.
float consoleDestroyProgress(int tileId,int heldItemId,bool underWater,
                             int digSpeedAmplifier=-1,int digSlowAmplifier=-1);
// Tile::playerDestroy -> spawnResources/getResource/getResourceCount and the
// shears/snow/leaf overrides. Callers must only request drops when
// consolePlayerCanDestroy was true before the block was removed.
// `experience` receives the popExperience amount (ores, redstone ore, spawners).
std::vector<SurvivalDrop> consoleTileDrops(int tileId,int data,int heldItemId,Random& random,int* experience=nullptr);
// DiggerItem/WeaponItem/ShearsItem::mineBlock durability cost for one block.
int consoleToolMineDamage(int itemId,int tileId);
// DiggerItem/WeaponItem/HoeItem::hurtEnemy durability cost for one hit.
int consoleToolAttackDamageCost(int itemId);
// Item::setMaxDamage values (Tier uses; shears 238). Zero is not damageable.
int consoleItemMaxDamage(int itemId);
// FoodConstants::EXHAUSTION_MINE, charged by Tile::playerDestroy.
float consoleMineExhaustion();

// FoodItem registrations from Item::staticCtor (FoodItem, BowlFoodItem,
// SeedFoodItem, GoldenAppleItem). Effect duration is in seconds, as passed
// to FoodItem::setEatEffect.
struct SurvivalFood {
    int nutrition=0;
    float saturationModifier=0;
    bool canAlwaysEat=false;
    int effectId=0,effectSeconds=0,effectAmplifier=0;
    float effectProbability=0;
    int returnsItem=0; // BowlFoodItem::useTimeDepleted
};
// Null when the item is not food.
const SurvivalFood* consoleFood(int itemId);
// FoodItem::EAT_DURATION in ticks.
constexpr int consoleEatDuration=32;
}
