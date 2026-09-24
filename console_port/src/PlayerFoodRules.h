#pragma once
#include "FoodData.h"

namespace console {
// Inputs supplied by the authoritative player, abilities and host-option system.
// Host options alone do not grant the matching per-player privilege.
struct PlayerFoodSettings {
    bool clientSide = false;
    bool invulnerable = false;
    bool flying = false;
    bool hostCanFly = false;
    bool canFly = false;
    bool hostCanChangeHunger = false;
    bool classicHunger = false;
    bool hostCanBeInvisible = false;
    bool invulnerablePrivilege = false;
    bool trustPlayers = true;
    bool cannotBuild = false;
};

class PlayerFoodRules {
    FoodData& foodData;
public:
    explicit PlayerFoodRules(FoodData& food) : foodData(food) {}
    PlayerFoodSettings settings;
    bool isAllowedToFly();
    bool isAllowedToIgnoreExhaustion();
    bool hasInvulnerablePrivilege();
    void causeFoodExhaustion(float amount);
    bool canEat(bool magicalItem);
};
}
