#include "CombatRules.h"
#include "World.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    using namespace console;
    require(sourceMobMeleeDamage(L"Zombie")==4 &&
            sourceMobMeleeDamage(L"Silverfish")==1 &&
            sourceMobMeleeDamage(L"Blaze")==6 &&
            sourceMobMeleeDamage(L"Enderman")==7 &&
            sourceMobMeleeDamage(L"PigZombie")==5 &&
            sourceMobMeleeDamage(L"Slime",1)==0 &&
            sourceMobMeleeDamage(L"Slime",4)==4 &&
            sourceMobMeleeDamage(L"LavaSlime",1)==3 &&
            sourceMobMeleeDamage(L"Skeleton")==0 &&
            sourceMobMeleeDamage(L"Creeper")==0,
            "Source creature melee values exclude projectile/fuse attacks");
    PlayerHurtState creative;
    require(!applySourcePlayerHurt(creative,4,2,true) && creative.health==20,
            "Creative invulnerability rejects mob melee damage");
    for(const auto& [difficulty,expected]:{
            std::pair{0,20},std::pair{1,17},std::pair{2,16},std::pair{3,14}}){
        PlayerHurtState player;
        const bool applied=applySourcePlayerHurt(player,4,difficulty,false);
        require(player.health==expected && applied==(difficulty!=0),
                "Mob attack follows source peaceful/easy/normal/hard damage scaling");
    }
    PlayerHurtState player;
    require(applySourcePlayerHurt(player,4,2,false) && player.health==16 &&
            player.invulnerableTicks==20 && player.hurtTicks==10,
            "First hurt opens a twenty-tick damage window");
    require(!applySourcePlayerHurt(player,4,2,false) && player.health==16,
            "Equal hit in first half of window is ignored");
    require(applySourcePlayerHurt(player,7,2,false) && player.health==13,
            "Stronger hit in first half deals only extra damage");
    for(int i=0;i<10;++i)tickSourcePlayerHurt(player);
    require(player.invulnerableTicks==10 &&
            applySourcePlayerHurt(player,4,2,false) && player.health==9,
            "Second half of window permits a full new hit");

    World world;world.generate(2045,true);
    const double ground=world.surface(80,80)+1;
    require(world.spawnCreativeEgg(54,{80.5,ground,80.5}),"Spawn melee Zombie");
    require(world.set(81,int(ground)+1,80,Stone),"Prepare line-of-sight wall");
    world.setPlayerPosition({82.3,ground,80.5});
    world.tickTime();
    require(world.entities().front().attackTicks==0 && world.playerHealth()==20,
            "Wall prevents Zombie melee acquisition");
    require(world.set(81,int(ground)+1,80,Air),"Open line of sight");
    world.tickTime();
    require(world.entities().front().attackTicks==0 && world.playerHealth()==20,
            "Source targeting excludes an invulnerable creative player entirely");
    const auto path=std::filesystem::temp_directory_path()/
        ("console-mob-melee-"+std::to_string(getpid())+".inner");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;
        std::filesystem::remove(path,ec);}} cleanup{path};
    world.save(path);
    World restored;
    require(restored.load(path) && restored.entities().front().attackTicks==0 &&
            restored.playerHealth()==20,
            "Creative player health and untargeted mob state survive save/reload");
    std::cout<<"Source mob melee damage, cooldown, occlusion, and creative guard passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
