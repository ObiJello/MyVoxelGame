#include "PlayerExperience.h"
#include "NbtIo.h"
#include <bit>
#include <cmath>
#include <stdexcept>
namespace console {
void PlayerExperience::applyXp(int i)
{
	// Update xp calculations from 1.3
	score = std::bit_cast<int>(std::uint32_t(score) + std::uint32_t(i));
	int max = INT_MAX - totalExperience;
	if (i > max)
	{
		i = max;
	}
	experienceProgress += (float) i / getXpNeededForNextLevel();
	totalExperience += i;
	while (experienceProgress >= 1)
	{
		experienceProgress = (experienceProgress - 1) * getXpNeededForNextLevel();
		levelUp();
		experienceProgress /= getXpNeededForNextLevel();
	}
}

void PlayerExperience::withdrawExperienceLevels(int amount)
{
	if (amount < 0) throw std::invalid_argument("Negative experience withdrawal");
    experienceLevel -= amount;
	if (experienceLevel < 0)
	{
		experienceLevel = 0;
	}
}

int PlayerExperience::getXpNeededForNextLevel()
{
	// Update xp calculations from 1.3
	if (experienceLevel >= 30)
	{
		return 17 + 15 * 3 + (experienceLevel - 30) * 7;
	}
	if (experienceLevel >= 15)
	{
		return 17 + (experienceLevel - 15) * 3;
	}
	return 17;
}

void PlayerExperience::levelUp()
{
	if (experienceLevel == maxLevel) throw std::overflow_error("Experience level cost exceeds console integer range");
    experienceLevel++;
}

int PlayerExperience::getExperienceReward()
{
	std::int64_t reward = std::int64_t(experienceLevel) * 7;
	if (reward > 100)
	{
		return 100;
	}
	return reward;
}

bool PlayerExperience::isAlwaysExperienceDropper()
{
	// players always drop experience
	return true;
}
void PlayerExperience::increaseXp(int amount)
{
    if (amount < 0) throw std::invalid_argument("Negative experience award");
    PlayerExperience next = *this;
    next.applyXp(amount);
    *this = next;
}

void PlayerExperience::readAdditionalSaveData(CompoundTag* entityTag) {
if (!entityTag) throw IoError("Missing experience save tag");
    PlayerExperience next = *this;
    next.experienceProgress = entityTag->getFloat(L"XpP");
    next.experienceLevel = entityTag->getInt(L"XpLevel");
    next.totalExperience = entityTag->getInt(L"XpTotal");
    if (!std::isfinite(next.experienceProgress) || next.experienceProgress < 0 || next.experienceProgress >= 1 ||
        next.experienceLevel < 0 || next.experienceLevel > maxLevel || next.totalExperience < 0)
        throw IoError("Invalid experience save state");
    *this = next;
}

void PlayerExperience::addAdditionalSaveData(CompoundTag* entityTag) {
if (!entityTag) throw IoError("Missing experience save tag");
entityTag->putFloat(L"XpP", experienceProgress);
	entityTag->putInt(L"XpLevel", experienceLevel);
	entityTag->putInt(L"XpTotal", totalExperience);
}

}
