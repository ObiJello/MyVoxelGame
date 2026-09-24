#include "World.h"
#include "PS3WorldStorage.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace console;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static Vec3 eyeBehind(const SimulatedEntity& mob,double eyeHeight){
    return {mob.position.x,mob.position.y+eyeHeight,mob.position.z-2.5};
}

int main(){try{
    const auto path=std::filesystem::temp_directory_path()/
        ("console-creature-combat-"+std::to_string(getpid())+".inner");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;
        std::filesystem::remove(path,ec);}} cleanup{path};
    World world;world.generate(1912,true);
    const double ground=world.surface(80,80)+1;
    require(world.spawnCreativeEgg(54,{80.5,ground,80.5}),"Spawn Zombie for attack test");
    require(world.entities().front().health==20,"Zombie starts with source health");
    const Vec3 eye=eyeBehind(world.entities().front(),1.4);
    require(!world.attackEntity(eye,{0,0,0},268),"Zero direction cannot attack");
    require(world.set(80,int(ground)+1,79,Stone),"Place occluding wall");
    require(!world.attackEntity(eye,{0,0,1},268) && world.entities().front().health==20,
            "A block in front prevents attacking through it");
    require(world.set(80,int(ground)+1,79,Air),"Remove occluding wall");
    require(world.attackEntity(eye,{0,0,1},268) && world.entities().front().health==16,
            "Wood sword deals source four damage");
    require(world.attackEntity(eye,{0,0,1},268) && world.entities().front().health==16,
            "Equal damage during source invulnerability does not stack");
    require(world.attackEntity(eye,{0,0,1},276) && world.entities().front().health==13,
            "Stronger hit during invulnerability deals only the excess");
    world.save(path);
    World reloaded;
    require(reloaded.load(path) && reloaded.entities().size()==1 &&
            reloaded.entities().front().health==13,
            "Simulated mob damage survives native save/reload");
    const auto newEye=eyeBehind(reloaded.entities().front(),1.4);
    require(reloaded.attackEntity(newEye,{0,0,1},276) &&
            reloaded.entities().front().health==6,
            "Diamond sword deals source seven damage after reload");
    reloaded.setPlayerPosition({64.5,ground,64.5});
    for(int i=0;i<20;++i)reloaded.tickTime();
    require(reloaded.attackEntity(eyeBehind(reloaded.entities().front(),1.4),{0,0,1},276) &&
            reloaded.entities().front().health==0,
            "A later attack kills the wounded mob");
    reloaded.save(path);
    World deadReload;
    require(deadReload.load(path),"Reload the death animation");
    bool deadZombie=false;
    for(const auto& entity:deadReload.entities())
        deadZombie|=entity.id==L"Zombie" && entity.health==0;
    require(deadZombie,
            "A saved death animation remains dead after reload");
    deadReload.setPlayerPosition({64.5,ground,64.5});
    for(int i=0;i<20;++i)deadReload.tickTime();
    bool zombieReturned=false;
    for(const auto& entity:deadReload.entities())zombieReturned|=entity.id==L"Zombie";
    int zombieXp=0;
    for(const auto& orb:deadReload.experienceOrbs())zombieXp+=orb.value;
    require(!zombieReturned && zombieXp==5,
            "The original twenty-tick death boundary releases Zombie XP once");
    deadReload.save(path);
    World orbReload;
    require(orbReload.load(path),"Reload pending Zombie experience");
    int savedZombieXp=0;
    for(const auto& orb:orbReload.experienceOrbs())savedZombieXp+=orb.value;
    require(savedZombieXp==5,
            "Defeated simulated mobs stay removed while their XP orb survives reload");

    World cowWorld;cowWorld.generate(1913,true);
    const double cowGround=cowWorld.surface(90,90)+1;
    require(cowWorld.spawnCreativeEgg(92,{90.5,cowGround,90.5}),
            "Spawn Cow for imported-NBT combat test");
    cowWorld.save(path);
    auto archive=PS3WorldStorage::readFile(path);
    auto chunk=archive->chunk(0,1,1);
    require(chunk && chunk->extra,"Cow owner chunk is available");
    auto* list=chunk->extra->getList(L"Entities");
    require(list && list->size()==1,"Cow has one source entity record");
    auto* tag=dynamic_cast<CompoundTag*>(list->get(0));
    require(tag,"Cow record is compound NBT");
    tag->putBoolean(L"console_port.simulated",false);
    tag->putString(L"ArchiveField",L"keep-me");
    archive->putChunk(0,*chunk);archive->writeFile(path);
    World native;
    require(native.load(path) && native.entities().size()==1 && native.entities().front().native,
            "Imported cow is active");
    require(native.attackEntity(eyeBehind(native.entities().front(),.8),{0,0,1},267) &&
            native.entities().front().health==4,
            "Iron sword damages imported cow without replacing its record");
    native.save(path);
    auto afterHit=PS3WorldStorage::readFile(path)->chunk(0,1,1);
    auto* saved=afterHit->extra->getList(L"Entities");
    auto* savedCow=dynamic_cast<CompoundTag*>(saved->get(0));
    auto* savedPos=savedCow?savedCow->getList(L"Pos"):nullptr;
    auto* savedZ=savedPos && savedPos->size()==3?
        dynamic_cast<DoubleTag*>(savedPos->get(2)):nullptr;
    require(saved && saved->size()==1 && savedCow && savedCow->getShort(L"Health")==4 &&
            savedCow->getString(L"ArchiveField")==L"keep-me" && savedZ &&
            std::abs(savedZ->data-(native.entities().front().position.z-64))<1e-6,
            "Native health and position persist while unrelated source NBT is preserved");
    native.setPlayerPosition({64.5,cowGround,64.5});
    for(int i=0;i<20;++i)native.tickTime();
    require(native.attackEntity(eyeBehind(native.entities().front(),.8),{0,0,1},276) &&
            native.entities().front().health==0,"Imported cow can die");
    native.save(path);
    auto afterDeath=PS3WorldStorage::readFile(path)->chunk(0,1,1);
    auto* remaining=afterDeath->extra->getList(L"Entities");
    bool deadCowSaved=false;
    if(remaining)for(int i=0;i<remaining->size();++i)
        if(auto* candidate=dynamic_cast<CompoundTag*>(remaining->get(i)))
            deadCowSaved|=candidate->getString(L"id")==L"Cow" &&
                          candidate->getShort(L"Health")==0 &&
                          candidate->getString(L"ArchiveField")==L"keep-me";
    require(remaining && deadCowSaved,
            "Native death animation and unrelated NBT survive a mid-death save");
    World nativeDeath;require(nativeDeath.load(path),"Reload imported cow death");
    bool nativeCowDead=false;
    for(const auto& entity:nativeDeath.entities())
        nativeCowDead|=entity.id==L"Cow" && entity.health==0;
    require(nativeCowDead,"Imported cow remains dead after reload");
    nativeDeath.setPlayerPosition({64.5,cowGround,64.5});
    for(int i=0;i<20;++i)nativeDeath.tickTime();
    nativeDeath.save(path);
    afterDeath=PS3WorldStorage::readFile(path)->chunk(0,1,1);
    remaining=afterDeath->extra->getList(L"Entities");
    bool cowReturned=false;
    if(remaining)for(int i=0;i<remaining->size();++i)
        if(auto* candidate=dynamic_cast<CompoundTag*>(remaining->get(i)))
            cowReturned|=candidate->getString(L"id")==L"Cow";
    require(remaining && !cowReturned && !nativeDeath.experienceOrbs().empty(),
            "Imported mob is removed and grants XP after the death animation");
    std::cout<<"Source attack damage, occlusion, cooldown, and native NBT passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
