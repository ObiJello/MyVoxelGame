#pragma once
#include <cstdint>
#include <string>
class CompoundTag;
namespace console {
// Ported from PotionBrewing's simplified formulas and
// BrewingStandTileEntity's four-slot tick rules.
bool isBrewingIngredient(int id);
int applyBrewingIngredient(int potionDamage,int ingredientId);
bool brewingCanStart(CompoundTag& tile);
bool tickBrewingStand(CompoundTag& tile);
int brewingBottleBits(CompoundTag& tile);
int brewingTime(CompoundTag& tile);
std::string potionDisplayName(int damage);
std::uint32_t potionColor(int damage);
}
