#pragma once

namespace console {
// The original FoodData calls into the player and food-item implementations.
// These explicit hooks allow its rules to run before the full entity/item port;
// the caller must supply real healing and starvation damage behavior.
class FoodPlayerAccess {
public:
    virtual ~FoodPlayerAccess() = default;
    virtual int difficulty() const = 0;
    virtual bool isAllowedToIgnoreExhaustion() const = 0;
    virtual bool isHurt() const = 0;
    virtual float getHealth() const = 0;
    virtual void heal(int amount) = 0;
    virtual void starve(int amount) = 0;
};

class FoodItemAccess {
public:
    virtual ~FoodItemAccess() = default;
    virtual int getNutrition() const = 0;
    virtual float getSaturationModifier() const = 0;
};
}
