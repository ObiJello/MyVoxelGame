#include "PlayerFoodRules.h"
#include <iostream>
#ifdef CONSOLE_PLAYER_FOOD_REFERENCE
#include "reference_shim/PlayerFoodReference.h"
using Policy=ReferenceFoodPolicy;
#else
using Policy=console::PlayerFoodRules;
#endif
int main() {
    for(int bits=0;bits<2048;++bits) for(int foodLevel : {0,19,20}) {
        FoodData food; food.setFoodLevel(foodLevel); Policy policy(food);
        auto& s=policy.settings;
        s.clientSide=bits&1; s.invulnerable=bits&2; s.flying=bits&4;
        s.hostCanFly=bits&8; s.canFly=bits&16; s.hostCanChangeHunger=bits&32;
        s.classicHunger=bits&64; s.hostCanBeInvisible=bits&128;
        s.invulnerablePrivilege=bits&256; s.trustPlayers=bits&512; s.cannotBuild=bits&1024;
        policy.causeFoodExhaustion(.25f);
        std::cout<<bits<<' '<<foodLevel<<' '<<policy.isAllowedToFly()<<' '
                 <<policy.isAllowedToIgnoreExhaustion()<<' '<<policy.hasInvulnerablePrivilege()<<' '
                 <<policy.canEat(false)<<' '<<policy.canEat(true)<<' '<<food.getExhaustionLevel()<<'\n';
    }
}
