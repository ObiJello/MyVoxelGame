#include "PlayerFoodRules.h"
#include <iostream>

static void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
int main() { try {
    FoodData food; console::PlayerFoodRules policy(food); auto& settings=policy.settings;
    require(!policy.canEat(false) && policy.canEat(true),"Magical food can be consumed when full");
    food.setFoodLevel(19); require(policy.canEat(false),"Normal food needs hunger");
    policy.causeFoodExhaustion(.2f); require(food.getExhaustionLevel()==.2f,"Server survival exhaustion");
    settings.clientSide=true; policy.causeFoodExhaustion(1); require(food.getExhaustionLevel()==.2f,"Client must not apply exhaustion");
    settings.clientSide=false; settings.hostCanFly=true;
    require(!policy.isAllowedToFly(),"Host flying option requires player privilege");
    settings.canFly=true; require(policy.isAllowedToFly() && !policy.isAllowedToIgnoreExhaustion(),"Flight privilege alone does not suppress hunger");
    settings.flying=true; policy.causeFoodExhaustion(1);
    require(policy.isAllowedToIgnoreExhaustion() && food.getExhaustionLevel()==.2f,"Flying with host permission suppresses hunger");
    settings.hostCanFly=false; require(!policy.isAllowedToIgnoreExhaustion(),"A flying flag without permission does not suppress hunger");
    settings.hostCanChangeHunger=true; settings.classicHunger=true;
    require(policy.isAllowedToIgnoreExhaustion(),"Classic hunger needs the matching host/player options");
    policy.causeFoodExhaustion(1); require(food.getExhaustionLevel()==.2f,"Classic hunger suppresses new exhaustion");
    settings.classicHunger=false; settings.invulnerable=true;
    policy.causeFoodExhaustion(1); require(food.getExhaustionLevel()==.2f && !policy.canEat(true),"Creative invulnerability suppresses exhaustion and food use");
    settings.invulnerable=false; settings.invulnerablePrivilege=true;
    require(!policy.hasInvulnerablePrivilege() && policy.canEat(false),"Invulnerable privilege requires original host visibility option");
    settings.hostCanBeInvisible=true; policy.causeFoodExhaustion(1);
    require(policy.hasInvulnerablePrivilege() && !policy.canEat(true) && food.getExhaustionLevel()==.2f,"Host invulnerable privilege blocks eating and exhaustion");
    settings.hostCanBeInvisible=false; settings.trustPlayers=false; settings.cannotBuild=true;
    policy.causeFoodExhaustion(1); require(food.getExhaustionLevel()==.2f,"TU6 untrusted cannot-build players retain hunger");
    settings.trustPlayers=true; policy.causeFoodExhaustion(1); require(food.getExhaustionLevel()==1.2f,"Trust option releases the cannot-build exhaustion guard");
    std::cout<<"Player food permissions and exhaustion authority checks passed\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; } }
