#pragma once
#include <vector>

namespace console {
// Source MobEffect ids and 20 Hz durations from PotionBrewing::getEffects.
struct PotionEffect {
    int id=0;
    int duration=0;
    int amplifier=0;
    bool operator==(const PotionEffect&) const = default;
};
std::vector<PotionEffect> potionEffects(int potionDamage);
void addPotionEffects(std::vector<PotionEffect>& active,const std::vector<PotionEffect>& incoming);
void tickPotionEffects(std::vector<PotionEffect>& active,int& health,bool invulnerable);
double potionMovementMultiplier(const std::vector<PotionEffect>& active);
int potionEffectRemaining(const std::vector<PotionEffect>& active,int id);
}
