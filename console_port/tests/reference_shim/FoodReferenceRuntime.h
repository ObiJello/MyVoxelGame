#pragma once
#include <memory>
#include <algorithm>
#include <stdexcept>
#include "Difficulty.h"
using std::shared_ptr;

// Only the independently compiled reference uses these stand-ins. They record
// the original FoodData's calls; none are linked into the client or native rules.
struct FoodReferenceLevel { int difficulty = Difficulty::NORMAL; };
struct DamageSource { inline static int starve = 1; };
class Player {
public:
    FoodReferenceLevel world;
    FoodReferenceLevel* level = &world;
    bool ignore = false;
    float health = 20;
    int healed = 0, starved = 0;
    bool isAllowedToIgnoreExhaustion() { return ignore; }
    bool isHurt() { return health > 0 && health < 20; }
    float getHealth() { return health; }
    void heal(int amount) { ++healed; health = std::min(20.0f, health + amount); }
    bool hurt(int source, int amount) { if(source != DamageSource::starve) throw std::runtime_error("Wrong damage source"); ++starved; health = std::max(0.0f, health - amount); return true; }
};
class FoodItem {
public:
    int food = 4;
    float saturation = .6f;
    int getNutrition() { return food; }
    float getSaturationModifier() { return saturation; }
};
