#include "World.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main(){try{
    using namespace console;
    World world;world.generate(1503,true);
    const double floor=world.surface(80,80)+1;
    require(world.spawnCreativeEgg(55,{80.5,floor,80.5}) &&
            world.spawnCreativeEgg(62,{84.5,floor,80.5}) &&
            world.spawnCreativeEgg(61,{110.5,floor+6,80.5}) &&
            world.spawnCreativeEgg(56,{65.5,floor+8,80.5}),
            "Source ground and flying mobs spawn in clear space");
    for(int x=94;x<=99;++x)for(int z=94;z<=99;++z)
        for(int y=int(floor);y<int(floor)+3;++y)
            require(world.set(x,y,z,Water),"Prepare a water volume for Squid");
    require(world.spawnCreativeEgg(94,{96.5,floor+.2,96.5}) &&
            world.spawnCreativeEgg(94,{105.5,floor+6,96.5}),
            "Squid eggs can spawn in water or air");
    const auto find=[&](const wchar_t* id,int occurrence=0)->const SimulatedEntity&{
        for(const auto& entity:world.entities())if(entity.id==id && occurrence--==0)return entity;
        throw std::runtime_error("Expected mob disappeared");
    };
    const int firstSlimeDelay=find(L"Slime").jumpDelay;
    const int firstMagmaDelay=find(L"LavaSlime").jumpDelay;
    require(firstSlimeDelay>=10 && firstSlimeDelay<=29 &&
            firstMagmaDelay>=10 && firstMagmaDelay<=29,
            "Slime constructor delay is 10-29 ticks even for Magma Cube");
    world.setPlayerPosition({88.5,floor,80.5});
    const double ghastStart=find(L"Ghast").position.y;
    double slimePeak=floor,magmaPeak=floor;
    int slimeRepeatDelay=-1,magmaRepeatDelay=-1;
    bool swimmingPulse=false;
    for(int tick=0;tick<38;++tick){
        world.tickTime();
        slimePeak=std::max(slimePeak,find(L"Slime").position.y);
        magmaPeak=std::max(magmaPeak,find(L"LavaSlime").position.y);
        if(slimeRepeatDelay<0 && find(L"Slime").position.y>floor+.3)
            slimeRepeatDelay=find(L"Slime").jumpDelay;
        if(magmaRepeatDelay<0 && find(L"LavaSlime").position.y>floor+.3)
            magmaRepeatDelay=find(L"LavaSlime").jumpDelay;
        swimmingPulse|=std::abs(find(L"Squid").squidTentacleAngle)>.05f;
    }
    require(slimePeak>floor+.3 && magmaPeak>floor+.3,
            "Both source slime classes jump from the ground after their delay");
    require(slimeRepeatDelay>=10 && slimeRepeatDelay<=29 &&
            magmaRepeatDelay>=40 && magmaRepeatDelay<=116,
            "Creative players are excluded from the shortened chase jump delays");
    require(find(L"Blaze").position.y<floor+6,
            "Blaze follows gravity and does not share Ghast flight");
    require(find(L"Ghast").position.y>floor+2 &&
            std::abs(find(L"Ghast").position.y-ghastStart)<6,
            "Ghast stays aloft under bounded target steering");
    require(swimmingPulse && find(L"Squid").squidPhase>0,
            "Underwater Squid advances a swimming pulse");
    require(find(L"Squid",1).position.y<floor+6 &&
            find(L"Squid",1).velocity.x==0 && find(L"Squid",1).velocity.z==0,
            "Squid outside water falls without a land-wander goal");
    std::cout<<"Source creature movement cadence and medium checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
