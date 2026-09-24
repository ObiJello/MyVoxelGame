#include "FoodAccess.h"
#include "NbtIo.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include "Difficulty.h"
#include "FoodConstants.h"
#include "FoodData.h"
FoodData::FoodData()
{
	exhaustionLevel = 0;
	tickTimer = 0;

	this->foodLevel = FoodConstants::MAX_FOOD;
	this->lastFoodLevel = FoodConstants::MAX_FOOD;
	this->saturationLevel = FoodConstants::START_SATURATION;
}

void FoodData::eat(int food, float saturationModifier)
{
	if (food < 0 || !std::isfinite(saturationModifier) || saturationModifier < 0)
        throw std::invalid_argument("Invalid food nutrition or saturation modifier");
    foodLevel = static_cast<int>(std::min<std::int64_t>(std::int64_t(food) + foodLevel, FoodConstants::MAX_FOOD));
	saturationLevel = min(saturationLevel + (float) food * saturationModifier * 2.0f, (float)foodLevel);
}

void FoodData::eat(const console::FoodItemAccess& item)
{
	eat(item.getNutrition(), item.getSaturationModifier());
}

void FoodData::tick(console::FoodPlayerAccess& player)
{

	int difficulty = player.difficulty();
    if (difficulty < Difficulty::PEACEFUL || difficulty > Difficulty::HARD)
        throw std::invalid_argument("Invalid food difficulty");

	lastFoodLevel = foodLevel;

	if (exhaustionLevel > FoodConstants::EXHAUSTION_DROP)
	{
		exhaustionLevel -= FoodConstants::EXHAUSTION_DROP;

		if (saturationLevel > 0)
		{
			saturationLevel = max(saturationLevel - 1, 0.0f);
		}
		else if (difficulty > Difficulty::PEACEFUL)
		{
			foodLevel = max(foodLevel - 1, 0);
		}
	}

	// 4J Added - Allow host to disable using hunger. We don't deplete the hunger bar due to exhaustion
	// but I think we should deplete it to heal
	if(player.isAllowedToIgnoreExhaustion())
	{
		if(foodLevel > 0 && player.isHurt())
		{
			tickTimer++;
			if (tickTimer >= FoodConstants::HEALTH_TICK_COUNT)
			{
				player.heal(1);
				--foodLevel;
				tickTimer = 0;
			}
		}
	}
	else if (foodLevel >= FoodConstants::HEAL_LEVEL && player.isHurt())
	{
		tickTimer++;
		if (tickTimer >= FoodConstants::HEALTH_TICK_COUNT)
		{
			player.heal(1);
			tickTimer = 0;
		}
	}
	else if (foodLevel <= FoodConstants::STARVE_LEVEL)
	{
		tickTimer++;
		if (tickTimer >= FoodConstants::HEALTH_TICK_COUNT)
		{
			if (player.getHealth() > 10 || difficulty >= Difficulty::HARD || (player.getHealth() > 1 && difficulty >= Difficulty::NORMAL))
			{
				player.starve(1);
			}
			tickTimer = 0;
		}
	}
	else
	{
		tickTimer = 0;
	}

}

void FoodData::readAdditionalSaveData(CompoundTag *entityTag)
{
    if (!entityTag) throw IoError("Missing food save tag");
    if (!entityTag->contains(L"foodLevel")) return;
    for (const auto* key : {L"foodLevel", L"foodTickTimer"})
        if (entityTag->contains(key) && entityTag->get(key)->getId() != Tag::TAG_Int)
            throw IoError("Invalid food integer tag");
    for (const auto* key : {L"foodSaturationLevel", L"foodExhaustionLevel"})
        if (entityTag->contains(key) && entityTag->get(key)->getId() != Tag::TAG_Float)
            throw IoError("Invalid food float tag");
    FoodData next = *this;
    next.setFoodLevel(entityTag->getInt(L"foodLevel"));
    next.tickTimer = entityTag->getInt(L"foodTickTimer");
    if (next.tickTimer < 0 || next.tickTimer >= FoodConstants::HEALTH_TICK_COUNT)
        throw IoError("Invalid food tick timer");
    next.setSaturation(entityTag->getFloat(L"foodSaturationLevel"));
    next.setExhaustion(entityTag->getFloat(L"foodExhaustionLevel"));
    *this = next;
}

void FoodData::addAdditonalSaveData(CompoundTag *entityTag)
{
	if (!entityTag) throw IoError("Missing food save tag");
    entityTag->putInt(L"foodLevel", foodLevel);
	entityTag->putInt(L"foodTickTimer", tickTimer);
	entityTag->putFloat(L"foodSaturationLevel", saturationLevel);
	entityTag->putFloat(L"foodExhaustionLevel", exhaustionLevel);
}

int FoodData::getFoodLevel()
{
	return foodLevel;
}

int FoodData::getLastFoodLevel()
{
	return lastFoodLevel;
}

bool FoodData::needsFood()
{
	return foodLevel < FoodConstants::MAX_FOOD;
}

void FoodData::addExhaustion(float amount)
{
	if (!std::isfinite(amount) || amount < 0)
        throw std::invalid_argument("Invalid exhaustion amount");
    exhaustionLevel = min(exhaustionLevel + amount, FoodConstants::MAX_SATURATION * 2);
}

float FoodData::getExhaustionLevel()
{
	return exhaustionLevel;
}

float FoodData::getSaturationLevel()
{
	return saturationLevel;
}

void FoodData::setFoodLevel(int food)
{
	if (food < 0 || food > FoodConstants::MAX_FOOD) throw std::invalid_argument("Invalid food level");
    this->foodLevel = food;
}

void FoodData::setSaturation(float saturation)
{
	if (!std::isfinite(saturation) || saturation < 0 || saturation > FoodConstants::MAX_SATURATION)
        throw std::invalid_argument("Invalid saturation level");
    this->saturationLevel = saturation;
}

void FoodData::setExhaustion(float exhaustion)
{
	if (!std::isfinite(exhaustion) || exhaustion < 0 || exhaustion > FoodConstants::MAX_SATURATION * 2)
        throw std::invalid_argument("Invalid exhaustion level");
    this->exhaustionLevel = exhaustion;
}
