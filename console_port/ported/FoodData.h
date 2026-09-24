#pragma once

#include "FoodAccess.h"
class CompoundTag;

class FoodData
{
private:
	int foodLevel;
	float saturationLevel;
	float exhaustionLevel;

	int tickTimer;
	int lastFoodLevel;

public:
	FoodData();

	void eat(int food, float saturationModifier);
	void eat(const console::FoodItemAccess& item);
	void tick(console::FoodPlayerAccess& player);
	void readAdditionalSaveData(CompoundTag *entityTag);
	void addAdditonalSaveData(CompoundTag *entityTag);
	int getFoodLevel();
	int getLastFoodLevel();
	bool needsFood();
	void addExhaustion(float amount);
	float getExhaustionLevel();
	float getSaturationLevel();
	void setFoodLevel(int food);
	void setSaturation(float saturation);
	void setExhaustion(float exhaustion);
};
