#include "World.h"
#include "PS3WorldStorage.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main(){try{
    using namespace console;
    const auto path=std::filesystem::temp_directory_path()/
        ("console-natural-mobs-"+std::to_string(getpid())+".inner");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);}} cleanup{path};
    World source;source.generate(487,true);source.save(path);
    World day;require(day.load(path),"Load flat daylight world");
    day.setPlayerPosition({64.5,4,64.5});
    for(int tick=0;tick<1000;++tick)day.tickTime();
    require(!day.entities().empty(),"Bright grass permits the source friendly-spawn cadence");
    for(const auto& mob:day.entities())
        require(mob.id==L"Cow" || mob.id==L"Pig" || mob.id==L"Sheep" || mob.id==L"Chicken",
                "Exposed flat grass prevents daylight monster spawning");
    auto archive=PS3WorldStorage::readFile(path);
    auto metadata=archive->metadata();metadata->setTime(18000);
    archive->putMetadata(*metadata);archive->writeFile(path);
    World night;require(night.load(path),"Load the same flat world at night");
    night.setPlayerPosition({64.5,4,64.5});
    const auto hostile=[](const SimulatedEntity& mob){
        return mob.id==L"Zombie" || mob.id==L"Skeleton" || mob.id==L"Spider" || mob.id==L"Creeper";
    };
    for(int tick=0;tick<6000;++tick){
        night.tickTime();
        if(std::any_of(night.entities().begin(),night.entities().end(),hostile))break;
    }
    require(std::any_of(night.entities().begin(),night.entities().end(),hostile),
            "Dark, loaded grass can naturally spawn enemies");
    for(const auto& mob:night.entities()){
        require(hostile(mob) || mob.id==L"Cow" || mob.id==L"Pig" || mob.id==L"Sheep" || mob.id==L"Chicken",
                "Natural entity has a ported model");
        require(mob.position.y>=4 && mob.position.y<256,"Natural enemy stands within the world");
        const auto dx=mob.position.x-64.5,dz=mob.position.z-64.5;
        require(dx*dx+dz*dz>=24*24,"Natural enemy respects the source player exclusion radius");
    }
    // Original archive entities should become visible without losing fields
    // that the smaller desktop simulation cannot serialize yet.
    auto imported=PS3WorldStorage::readFile(path);
    auto importedMetadata=imported->metadata();importedMetadata->setTime(0);
    imported->putMetadata(*importedMetadata);
    auto chunk=imported->chunk(0,-2,-2);
    require(chunk && chunk->extra,"Native entity owner chunk exists");
    auto entries=std::make_unique<TagList>();
    auto original=std::make_unique<CompoundTag>();
    original->putString(L"id",L"Zombie");original->putShort(L"Health",20);
    original->putString(L"ArchiveField",L"preserve");
    auto numbers=[](double x,double y,double z){
        auto list=std::make_unique<TagList>();
        for(double number:{x,y,z}){auto tag=std::make_unique<DoubleTag>(L"",number);list->add(tag.get());tag.release();}
        return list;
    };
    auto pos=numbers(-23.5,4,-23.5),motion=numbers(0,0,0);
    original->put(L"Pos",pos.get());pos.release();
    original->put(L"Motion",motion.get());motion.release();
    entries->add(original.get());original.release();
    auto sheep=std::unique_ptr<CompoundTag>(dynamic_cast<CompoundTag*>(entries->get(0)->copy()));
    sheep->putString(L"id",L"Sheep");sheep->putByte(L"Color",14);
    sheep->putBoolean(L"Sheared",false);
    auto sheepPos=numbers(-21.5,4,-23.5);sheep->put(L"Pos",sheepPos.get());sheepPos.release();
    entries->add(sheep.get());sheep.release();
    chunk->extra->put(L"Entities",entries.get());entries.release();
    imported->putChunk(0,*chunk);imported->writeFile(path);
    World withNative;require(withNative.load(path),"Load original archive mob");
    require(withNative.entities().size()==2 && withNative.entities().front().native &&
            withNative.entities().front().id==L"Zombie","Original console mob enters visible simulation");
    require(withNative.entities()[1].id==L"Sheep" && withNative.entities()[1].woolColor==14 &&
            !withNative.entities()[1].sheared,"Native sheep dye and fleece state reach the renderer");
    withNative.setPlayerPosition({44.5,4,40.5});
    withNative.set(41,4,40,Stone);withNative.set(41,5,40,Stone);
    require(withNative.attackEntity({39,5.4,40.5},{1,0,0},0),
            "A hit applies source knockback toward the wall");
    for(int tick=0;tick<19;++tick)withNative.tickTime();
    require(withNative.entities().front().position.x<=40.701,"Mob knockback respects a two-block wall");
    withNative.set(41,4,40,Air);withNative.set(41,5,40,Air);
    require(withNative.attackEntity({39,5.4,40.5},{1,0,0},0),
            "A second hit applies knockback in open space");
    for(int tick=0;tick<20;++tick)withNative.tickTime();
    require(withNative.entities().front().position.x>40.701,"Mob knockback advances in open space");
    require(std::abs(withNative.entities()[1].position.x-42.5)<4,
            "Passive sheep remain near their grazing area during the short collision check");
    withNative.save(path);
    auto preserved=PS3WorldStorage::readFile(path)->chunk(0,-2,-2);
    auto* saved=preserved->extra->getList(L"Entities");
    require(saved && saved->size()==2,"Saving native mobs does not duplicate them");
    auto* savedMob=dynamic_cast<CompoundTag*>(saved->get(0));
    require(savedMob && savedMob->getString(L"ArchiveField")==L"preserve" &&
            !savedMob->getBoolean(L"console_port.simulated"),"Native mob NBT remains intact");
    std::cout<<"Day/night natural monster spawning and loaded-world exclusion passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
