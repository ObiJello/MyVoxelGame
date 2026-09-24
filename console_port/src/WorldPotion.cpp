#include "WorldState.h"
#include "LevelSettings.h"
#include "FoodAccess.h"
#include <algorithm>
namespace console {
namespace {
class WorldFoodPlayer final : public FoodPlayerAccess {
    PlayerHurtState& hurt;
    bool creative;
public:
    WorldFoodPlayer(PlayerHurtState& state,bool invulnerable):hurt(state),creative(invulnerable){}
    int difficulty()const override{return 2;}
    bool isAllowedToIgnoreExhaustion()const override{return false;}
    bool isHurt()const override{return hurt.health>0 && hurt.health<20;}
    float getHealth()const override{return float(hurt.health);}
    void heal(int amount)override{hurt.health=std::min(20,hurt.health+amount);}
    void starve(int amount)override{
        if(!creative)hurt.health=std::max(0,hurt.health-amount);
    }
};
}
bool World::drinkPotion(int damage){
    if(damage<0 || damage>32767 || (damage&0x4000)!=0)return false;
    const bool invulnerable=state->metadata->getGameType()==GameType::CREATIVE;
    for(const auto& effect:potionEffects(damage)){
        if(effect.id==6 || effect.id==7){
            std::vector<PotionEffect> instant{effect};
            tickPotionEffects(instant,state->playerHurt.health,invulnerable);
        }else addPotionEffects(state->playerEffects,{effect});
    }
    return true;
}
const std::vector<PotionEffect>& World::activePotionEffects()const{return state->playerEffects;}
int World::potionEffectDuration(int id)const{return potionEffectRemaining(state->playerEffects,id);}
double World::potionSpeedMultiplier()const{return potionMovementMultiplier(state->playerEffects);}
int World::playerHealth()const{return state->playerHurt.health;}
int World::playerFoodLevel()const{return state->playerFood.getFoodLevel();}
float World::playerSaturation()const{return state->playerFood.getSaturationLevel();}
int World::playerExperienceLevel()const{return state->playerExperience.getLevel();}
int World::playerTotalExperience()const{return state->playerExperience.getTotal();}
void World::tickPlayerEffects(){
    const bool invulnerable=state->metadata->getGameType()==GameType::CREATIVE;
    tickSourcePlayerHurt(state->playerHurt);
    tickPotionEffects(state->playerEffects,state->playerHurt.health,invulnerable);
    WorldFoodPlayer player(state->playerHurt,invulnerable);
    state->playerFood.tick(player);
}
}
