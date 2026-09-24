#pragma once
#include <cstdint>
#include <limits>
class CompoundTag;

namespace console {
// Original Player XP state and operations. Entity pickup/death dispatch and the
// inventory/enchantment callers remain separate ports; no entity is synthesized.
class PlayerExperience {
    int score = 0;
    int experienceLevel = 0;
    int totalExperience = 0;
    float experienceProgress = 0;
    void applyXp(int amount);
public:
    static constexpr int maxLevel = 30 + (std::numeric_limits<int>::max() - 62) / 7;
    int getScore() const { return score; }
    int getLevel() const { return experienceLevel; }
    int getTotal() const { return totalExperience; }
    float getProgress() const { return experienceProgress; }
    void increaseXp(int amount);
    void withdrawExperienceLevels(int amount);
    int getXpNeededForNextLevel();
    void levelUp();
    int getExperienceReward();
    bool isAlwaysExperienceDropper();
    void readAdditionalSaveData(CompoundTag* tag);
    void addAdditionalSaveData(CompoundTag* tag);
};
}
