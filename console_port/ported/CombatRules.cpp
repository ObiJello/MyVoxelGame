#include "CombatRules.h"
#include <algorithm>

namespace console {
int sourceMobMeleeDamage(std::wstring_view id,int slimeSize){
    if(id==L"Zombie")return 4;
    if(id==L"Blaze")return 6;
    if(id==L"Enderman")return 7;
    if(id==L"PigZombie")return 5;
    if(id==L"Silverfish")return 1;
    if(id==L"Slime")return slimeSize>1?slimeSize:0;
    if(id==L"LavaSlime")return slimeSize+2;
    if(id==L"Skeleton" || id==L"Creeper" || id==L"Ghast")return 0;
    if(id==L"Spider" || id==L"CaveSpider")return 2;
    return 0;
}
bool applySourcePlayerHurt(PlayerHurtState& player,int damage,int difficulty,bool invulnerable){
    if(invulnerable || player.health<=0 || damage<=0)return false;
    // DamageSource::mobAttack scales with the source Level difficulty.
    if(difficulty<=0)return false;
    if(difficulty==1)damage=damage/2+1;
    else if(difficulty>=3)damage=damage*3/2;
    if(player.invulnerableTicks>10){
        if(damage<=player.lastHurt)return false;
        player.health=std::max(0,player.health-(damage-player.lastHurt));
    }else{
        player.health=std::max(0,player.health-damage);
        player.invulnerableTicks=20;
        player.hurtTicks=10;
    }
    player.lastHurt=damage;
    return true;
}
void tickSourcePlayerHurt(PlayerHurtState& player){
    if(player.invulnerableTicks>0)--player.invulnerableTicks;
    if(player.hurtTicks>0)--player.hurtTicks;
    if(player.health<=0)++player.deathTicks;
}
}
