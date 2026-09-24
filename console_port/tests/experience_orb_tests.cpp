#include "World.h"
#include "ExperienceOrbRules.h"
#include "ExperienceOrbMesh.h"
#include "PS3WorldStorage.h"
#include "FurnaceSimulation.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    using namespace console;
    for(const auto& [remaining,expected]:{
        std::pair{1,1},std::pair{3,3},std::pair{6,3},std::pair{7,7},
        std::pair{17,17},std::pair{37,37},std::pair{2477,2477},
        std::pair{2500,2477}})
        require(sourceExperienceOrbValue(remaining)==expected,
                "XP rewards split at the source orb-value thresholds");
    require(sourceExperienceOrbIcon(1)==0 && sourceExperienceOrbIcon(3)==1 &&
            sourceExperienceOrbIcon(7)==2 && sourceExperienceOrbIcon(2477)==10 &&
            sourceMobExperienceReward(L"Blaze",1,0)==10 &&
            sourceMobExperienceReward(L"Zombie",1,0)==5 &&
            sourceMobExperienceReward(L"Slime",4,0)==4 &&
            sourceMobExperienceReward(L"Chicken",1,2)==3 &&
            sourceMobExperienceReward(L"Villager",1,0)==0,
            "Source sprite frames and creature rewards are selected correctly");
    ExperienceOrbState display;display.position={8,70,9};display.value=7;
    const auto quad=buildExperienceOrbMesh(display,0,0,(10<<20)|(4<<4));
    require(quad.size()==6 && std::abs(quad[0].u-.5f)<1e-6f &&
            std::abs(quad[0].v-.25f)<1e-6f &&
            std::abs(quad[0].lightU-11.5f/16)<1e-6f,
            "Orb billboard uses the source 64x64 sprite and boosted light");
    World world;world.generate(616,true);
    const double ground=world.surface(80,80)+1;
    require(world.spawnCreativeEgg(93,{80.5,ground,80.5}),"Spawn Chicken for XP test");
    world.setPlayerPosition({64.5,ground,64.5});
    require(world.attackEntity({80.5,ground+.4,78.0},{0,0,1},276) &&
            world.entities().front().health==0,
            "Player kill records a pending source reward");
    for(int i=0;i<19;++i)world.tickTime();
    require(world.experienceOrbs().empty(),"XP is held during the death animation");
    world.tickTime();
    int reward=0;
    for(const auto& orb:world.experienceOrbs())reward+=orb.value;
    require(!world.experienceOrbs().empty() && reward>=1 && reward<=3,
            "Animal death creates its randomized source reward");
    const auto orb=world.experienceOrbs().front();
    world.setPlayerPosition({orb.position.x,ground,orb.position.z});
    for(int i=0;i<100 && !world.experienceOrbs().empty();++i)world.tickTime();
    require(world.experienceOrbs().empty() && world.playerTotalExperience()==reward,
            "Player pickup consumes the orb and applies its exact XP value");
    for(int i=0;i<3;++i)world.tickTime();
    require(world.playerTotalExperience()==reward,"A collected orb cannot award XP twice");
    const auto path=std::filesystem::temp_directory_path()/
        ("console-experience-orb-"+std::to_string(getpid())+".inner");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;
        std::filesystem::remove(path,ec);}} cleanup{path};
    world.save(path);World loaded;
    require(loaded.load(path) && loaded.playerTotalExperience()==reward,
            "Picked-up experience survives a native world save");
    auto archive=PS3WorldStorage::readFile(path);
    auto chunk=archive->chunk(0,1,1);
    require(chunk && chunk->extra,"Orb import chunk is available");
    auto* entities=dynamic_cast<TagList*>(chunk->extra->get(L"Entities"));
    if(!entities){chunk->extra->put(L"Entities",new TagList());
        entities=chunk->extra->getList(L"Entities");}
    auto nativeOrb=std::make_unique<CompoundTag>();
    nativeOrb->putString(L"id",L"XPOrb");nativeOrb->putShort(L"Health",5);
    nativeOrb->putShort(L"Age",0);nativeOrb->putShort(L"Value",7);
    nativeOrb->putString(L"ForeignOrbField",L"keep");
    for(const auto& [key,values]:{
            std::pair{L"Pos",std::array<double,3>{16.5,ground,16.5}},
            std::pair{L"Motion",std::array<double,3>{0,0,0}}}){
        auto list=std::make_unique<TagList>();
        for(double value:values){auto axis=std::make_unique<DoubleTag>(L"",value);
            list->add(axis.get());axis.release();}
        nativeOrb->put(key,list.get());list.release();
    }
    entities->add(nativeOrb.get());nativeOrb.release();
    archive->putChunk(0,*chunk);archive->writeFile(path);
    World imported;
    require(imported.load(path) && imported.experienceOrbs().size()==1 &&
            imported.experienceOrbs().front().native &&
            imported.experienceOrbs().front().value==7,
            "Original XPOrb NBT is instantiated as a collectible orb");
    imported.save(path);
    archive=PS3WorldStorage::readFile(path);chunk=archive->chunk(0,1,1);
    entities=chunk->extra->getList(L"Entities");
    bool preserved=false;
    for(int i=0;i<entities->size();++i)
        if(auto* tag=dynamic_cast<CompoundTag*>(entities->get(i)))
            preserved|=tag->getString(L"id")==L"XPOrb" &&
                       tag->getString(L"ForeignOrbField")==L"keep";
    require(preserved,"Native orb save preserves unrelated source NBT");
    imported.setPlayerPosition({80.5,ground,80.5});
    for(int i=0;i<10 && !imported.experienceOrbs().empty();++i)imported.tickTime();
    require(imported.experienceOrbs().empty() &&
            imported.playerTotalExperience()==reward+7,
            "Imported orb pickup grants its saved value");
    imported.save(path);archive=PS3WorldStorage::readFile(path);
    entities=archive->chunk(0,1,1)->extra->getList(L"Entities");
    bool returned=false;
    for(int i=0;i<entities->size();++i)
        if(auto* tag=dynamic_cast<CompoundTag*>(entities->get(i)))
            returned|=tag->getString(L"id")==L"XPOrb";
    require(!returned,"Collected imported orbs cannot resurrect after save");
    require(furnaceExperienceValue(266)==1.f &&
            furnaceExperienceValue(265)==.7f &&
            furnaceExperienceValue(331)==.7f &&
            furnaceExperienceValue(263)==.1f &&
            furnaceExperienceValue(351)==.2f &&
            furnaceExperienceValue(406)==.2f,
            "Smelting XP uses the source output-ID reward table");
    World furnaceWorld;furnaceWorld.generate(617,true);
    require(furnaceWorld.placeBlock(80,200,80,static_cast<Block>(61),0,{75,200,75},0),
            "Place furnace for reward integration");
    furnaceWorld.save(path);archive=PS3WorldStorage::readFile(path);
    chunk=archive->chunk(0,1,1);
    bool outputSeeded=false;
    if(auto* tiles=dynamic_cast<TagList*>(chunk->extra->get(L"TileEntities")))
        for(int i=0;i<tiles->size();++i)
            if(auto* tile=dynamic_cast<CompoundTag*>(tiles->get(i)))
                if(tile->getString(L"id")==L"Furnace" &&
                   tile->getInt(L"x")==16 && tile->getInt(L"z")==16){
                    auto* items=dynamic_cast<TagList*>(tile->get(L"Items"));
                    if(!items){tile->put(L"Items",new TagList());items=tile->getList(L"Items");}
                    auto gold=std::make_unique<CompoundTag>();
                    gold->putByte(L"Slot",2);gold->putShort(L"id",266);
                    gold->putByte(L"Count",2);gold->putShort(L"Damage",0);
                    items->add(gold.get());gold.release();outputSeeded=true;
                }
    require(outputSeeded,"Native furnace result slot is available");
    archive->putChunk(0,*chunk);archive->writeFile(path);
    require(furnaceWorld.load(path),"Load furnace result fixture");
    furnaceWorld.setPlayerPosition({80.5,200,80.5});
    require(furnaceWorld.transferFurnaceItem(80,200,80,2,true),
            "Take smelted gold from the source output slot");
    int furnaceXp=0;
    for(const auto& pending:furnaceWorld.experienceOrbs())furnaceXp+=pending.value;
    require(furnaceXp==2 && furnaceWorld.playerTotalExperience()==0,
            "Taking two gold ingots spawns two XP rather than awarding it instantly");
    furnaceWorld.save(path);World furnaceReload;
    require(furnaceReload.load(path) && furnaceReload.experienceOrbs().size()==2,
            "Furnace result XP orbs survive the native save path");
    std::cout<<"Source orb values, mesh, death reward, pickup and save passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
