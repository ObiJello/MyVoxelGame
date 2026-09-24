#include "PotionEffects.h"
#include "BrewingSimulation.h"
#include "World.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace console;
static void require(bool condition,const char* label){if(!condition)throw std::runtime_error(label);}
int main(){try{
    require(potionEffects(0).empty() && potionEffects(16).empty(),
            "Water and awkward potions have no effects");
    struct Case {int damage,id,duration,amplifier;};
    for(const auto& sample:std::array{
        Case{8193,10,900,0},Case{8194,1,3600,0},Case{8195,12,3600,0},
        Case{8196,19,900,0},Case{8197,6,1,0},Case{8198,16,3600,0},
        Case{8200,18,1800,0},Case{8201,5,3600,0},Case{8202,2,1800,0},
        Case{8204,7,1,0},Case{8206,14,3600,0},
        Case{8258,1,9600,0},Case{8226,1,1800,1},Case{8229,6,1,1},
    }){
        const auto effects=potionEffects(sample.damage);
        require(effects.size()==1 && effects[0]==PotionEffect{sample.id,sample.duration,sample.amplifier},
                "Source effect type, 20Hz duration, and amplifier");
    }
    const auto splash=potionEffects(8194|0x4000);
    require(splash.size()==1 && splash[0].id==1 && splash[0].duration==2701,
            "Source splash duration adjustment");
    require(applyBrewingIngredient(16,353)==8194 &&
            potionEffects(applyBrewingIngredient(16,353))[0].id==1,
            "Brewing result feeds source effect formulas");
    std::vector<PotionEffect> active;
    addPotionEffects(active,potionEffects(8194));
    require(active.size()==1 && potionMovementMultiplier(active)==1.2,
            "Speed applies the source 20 percent movement bonus");
    addPotionEffects(active,potionEffects(8258));
    require(active[0].duration==9600,"Equal amplifier keeps longer duration");
    addPotionEffects(active,potionEffects(8226));
    require(active[0].amplifier==1 && active[0].duration==1800 &&
            potionMovementMultiplier(active)==1.4,"Stronger amplifier replaces weaker effect");
    addPotionEffects(active,potionEffects(8194));
    require(active[0].amplifier==1 && active[0].duration==1800,
            "Weaker potion does not replace a stronger effect");
    addPotionEffects(active,potionEffects(8202));
    require(active.size()==2 && std::abs(potionMovementMultiplier(active)-1.19)<1e-9,
            "Slowness multiplies speed after the speed bonus");
    int health=19;std::vector<PotionEffect> regen=potionEffects(8193);
    tickPotionEffects(regen,health,false);
    require(health==20 && regen[0].duration==899,"Regeneration heals on the source tick interval");
    health=20;auto poison=potionEffects(8196);tickPotionEffects(poison,health,false);
    require(health==19,"Poison damages a vulnerable player");
    health=20;poison=potionEffects(8196);tickPotionEffects(poison,health,true);
    require(health==20,"Creative invulnerability blocks poison damage");
    auto healing=potionEffects(8229);health=6;tickPotionEffects(healing,health,false);
    require(health==18 && healing.empty(),"Instant healing applies then expires");
    require(potionEffectRemaining(active,1)==1800 && potionEffectRemaining(active,16)==0,
            "Remaining durations can drive renderer effects");
    auto scratch=(std::filesystem::temp_directory_path()/"console-potion-XXXXXX").string();
    std::vector<char> name(scratch.begin(),scratch.end());name.push_back(0);
    require(mkdtemp(name.data())!=nullptr,"Create world persistence scratch directory");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}}cleanup{name.data()};
    const auto save=cleanup.path/"world.inner";
    World world;world.generate(42,true);
    require(world.drinkPotion(0) && world.activePotionEffects().empty(),
            "Water bottle can be used without adding effects");
    require(!world.drinkPotion(8194|0x4000),"Splash potion cannot be drunk");
    require(world.drinkPotion(8194) && world.potionEffectDuration(1)==3600 &&
            world.potionSpeedMultiplier()==1.2,"Drinking a brewed speed potion changes player movement");
    require(world.drinkPotion(8198) && world.potionEffectDuration(16)==3600,
            "Night vision is available to the lightmap");
    world.tickTime();
    require(world.potionEffectDuration(1)==3599 && world.potionEffectDuration(16)==3599,
            "Active effects advance on world ticks");
    world.save(save);World restored;
    require(restored.load(save) && restored.potionEffectDuration(1)==3599 &&
            restored.potionEffectDuration(16)==3599 && restored.playerHealth()==20,
            "Active effects and health survive a native world save");
    restored.tickTime();
    require(restored.potionEffectDuration(1)==3598,
            "Loaded effects resume ticking");
    std::cout<<"potion effect tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
