#pragma once
#include "FoodData.h"
#include "FoodConstants.h"
#include "NbtIo.h"
#include <algorithm>
#ifdef CONSOLE_FOOD_REFERENCE
#include "FoodReferenceRuntime.h"
struct FoodFixture {
    std::shared_ptr<Player> player = std::make_shared<Player>();
    Player& state() { return *player; }
    void difficulty(int value) { player->world.difficulty = value; }
    void tick(FoodData& food) { food.tick(player); }
    void eatItem(FoodData& food) { FoodItem item; food.eat(&item); }
};
#else
#include "FoodAccess.h"
struct FoodFixture : console::FoodPlayerAccess {
    int mode = 2;
    bool ignore = false;
    float health = 20;
    int healed = 0, starved = 0;
    FoodFixture& state() { return *this; }
    void difficulty(int value) { mode = value; }
    int difficulty() const override { return mode; }
    bool isAllowedToIgnoreExhaustion() const override { return ignore; }
    bool isHurt() const override { return health > 0 && health < 20; }
    float getHealth() const override { return health; }
    void heal(int amount) override { ++healed; health = std::min(20.0f, health + amount); }
    void starve(int amount) override { ++starved; health = std::max(0.0f, health - amount); }
    void tick(FoodData& food) { food.tick(*this); }
    void eatItem(FoodData& food) {
        struct Item : console::FoodItemAccess {
            int getNutrition() const override { return 4; }
            float getSaturationModifier() const override { return .6f; }
        } item;
        food.eat(item);
    }
};
#endif
