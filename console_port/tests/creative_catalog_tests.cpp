#include "CreativeCatalog.h"
#include "ItemIcons.h"
#include "ItemNames.h"
#include "ItemPlacement.h"
#include "PS3WorldStorage.h"
#include "SpawnEggColors.h"
#include "World.h"
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

using namespace console;
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){try{
    require(creativeTabCount()==8,"Original PS3 creative menu has eight tabs");
    constexpr std::array expectedSizes{78,70,24,46,24,55,68,58};
    constexpr std::array<std::string_view,8> names{"Building Blocks","Decoration",
        "Redstone & Transport","Materials","Food","Tools & Armour","Brewing","Miscellaneous"};
    int total=0;
    for(int i=0;i<creativeTabCount();++i){
        const auto tab=creativeTab(i);
        require(tab.name==names[i] && int(tab.items.size())==expectedSizes[i],
                "Creative tab order and source item count");
        total+=int(tab.items.size());
        for(const auto& item:tab.items)
            require(item.id>0 && item.id<=32767 && item.damage>=0 && item.damage<=32767,
                    "Creative item value range");
    }
    require(total==423,"All active source catalog entries are available");
    for(int tabIndex=0;tabIndex<creativeTabCount();++tabIndex)
        for(const auto& entry:creativeTab(tabIndex).items)
            require(consoleSourceItemName(entry.id,entry.damage)!=nullptr,
                    "Every creative entry has a source display name");
    require(std::string_view(consoleSourceItemName(5,1))=="Spruce Wood Planks" &&
            std::string_view(consoleSourceItemName(35,0))=="White Wool" &&
            std::string_view(consoleSourceItemName(35,15))=="Black Wool" &&
            std::string_view(consoleSourceItemName(351,15))=="Bone Meal" &&
            std::string_view(consoleSourceItemName(397,1))=="Wither Skeleton Skull" &&
            std::string_view(consoleSourceItemName(263,1))=="Charcoal" &&
            std::string_view(consoleSourceItemName(383,50))=="Spawn Creeper" &&
            std::string_view(consoleSourceItemName(383,120))=="Spawn Villager",
            "Variant names use the console source metadata rules");
    require(creativeTab(0).items[0]==CreativeEntry{1,0} &&
            creativeTab(0).items[6]==CreativeEntry{24,2} &&
            creativeTab(0).items.back()==CreativeEntry{156,0},
            "Building blocks retain source ordering and variants");
    require(creativeTab(1).items.front()==CreativeEntry{397,0} &&
            creativeTab(3).items[1]==CreativeEntry{263,1} &&
            creativeTab(4).items[2]==CreativeEntry{322,1},
            "Decoration, material, and food metadata variants");
    require(consoleItemIcon(397,0).tile==224 && consoleItemIcon(397,4).tile==228,
            "Every skull variant has its source item-atlas icon");
    const auto creeperEgg=consoleSourceEggColors(50);
    const auto villagerEgg=consoleSourceEggColors(120);
    require(creeperEgg.valid && creeperEgg.base==0x0da70b && creeperEgg.spots==0x000000 &&
            villagerEgg.valid && villagerEgg.base==0x563c33 && villagerEgg.spots==0xbd8b72 &&
            std::wstring_view(creeperEgg.entityName)==L"Creeper" &&
            !consoleSourceEggColors(999).valid,
            "Spawn egg layers use EntityIO and PS3 color-table values");
    require(placedTileForItem(379)==117 && placedTileForItem(380)==118 &&
            placedTileForItem(287)==132 && placedTileForItem(338)==83 &&
            placedTileForItem(373)==0,"Source tile-planter item mapping");
    World placement;placement.generate(87,true);
    const double spawnY=placement.surface(64,64)+1;
    require(placement.spawnCreativeEgg(50,{64.5,spawnY,64.5}) &&
            placement.spawnCreativeEgg(57,{66.5,spawnY,64.5}) &&
            placement.spawnCreativeEgg(59,{68.5,spawnY,64.5}) &&
            placement.spawnCreativeEgg(60,{70.5,spawnY,64.5}) &&
            placement.spawnCreativeEgg(120,{72.5,spawnY,64.5}) &&
            placement.spawnCreativeEgg(98,{74.5,spawnY,64.5}) &&
            placement.spawnCreativeEgg(95,{76.5,spawnY,64.5}) &&
            placement.entities().size()==7 && placement.entities()[0].id==L"Creeper" &&
            placement.entities()[1].id==L"PigZombie" &&
            placement.entities()[2].id==L"CaveSpider" &&
            placement.entities()[3].id==L"Silverfish" &&
            placement.entities()[4].id==L"Villager" &&
            placement.entities()[5].id==L"Ozelot" && placement.entities()[5].catType==0 &&
            placement.entities()[6].id==L"Wolf" && !placement.entities()[6].wolfTame &&
            placement.entities()[6].collarColor==14 &&
            !placement.spawnCreativeEgg(999,{65.5,spawnY,64.5}) &&
            !placement.spawnCreativeEgg(50,{64.5,spawnY-1,64.5}),
            "Modeled creative spawn eggs create entities but reject unsupported or blocked spawns");
    const int profession=placement.entities()[4].profession;
    require(profession>=0 && profession<5,"Villager egg finalizes one of five source professions");
    const auto eggSave=std::filesystem::temp_directory_path()/
        ("console-creative-egg-"+std::to_string(getpid())+".inner");
    struct SaveCleanup{std::filesystem::path path;~SaveCleanup(){
        std::error_code ignored;std::filesystem::remove(path,ignored);
    }} eggSaveCleanup{eggSave};
    placement.save(eggSave);
    World eggReload;
    require(eggReload.load(eggSave) && eggReload.entities().size()==7 &&
            eggReload.entities()[0].id==L"Creeper" &&
            eggReload.entities()[1].id==L"PigZombie" &&
            eggReload.entities()[2].id==L"CaveSpider" &&
            eggReload.entities()[3].id==L"Silverfish" &&
            eggReload.entities()[4].id==L"Villager" &&
            eggReload.entities()[4].profession==profession &&
            eggReload.entities()[5].id==L"Ozelot" && eggReload.entities()[5].catType==0 &&
            eggReload.entities()[6].id==L"Wolf" && eggReload.entities()[6].collarColor==14,
            "Creative spawn-egg mobs survive a native save/reload");
    auto nativeArchive=PS3WorldStorage::readFile(eggSave);
    auto nativeChunk=nativeArchive->chunk(0,0,0);
    require(nativeChunk && nativeChunk->extra,"Saved egg mobs have an owner chunk");
    auto* nativeMobs=nativeChunk->extra->getList(L"Entities");
    require(nativeMobs,"Saved egg mobs have native entity records");
    for(int i=0;i<nativeMobs->size();++i){
        auto* tag=dynamic_cast<CompoundTag*>(nativeMobs->get(i));
        if(!tag)continue;
        if(tag->getString(L"id")==L"Ozelot"){
            tag->putBoolean(L"console_port.simulated",false);
            tag->putInt(L"CatType",3);
        }
        if(tag->getString(L"id")==L"Wolf"){
            tag->putBoolean(L"console_port.simulated",false);
            tag->putString(L"Owner",L"source-owner");
            tag->putBoolean(L"Sitting",true);
            tag->putBoolean(L"Angry",false);
            tag->putByte(L"CollarColor",11);
        }
    }
    nativeArchive->putChunk(0,*nativeChunk);nativeArchive->writeFile(eggSave);
    World nativePets;require(nativePets.load(eggSave),"Native cat and wolf records reload");
    require(nativePets.entities().size()==7 && nativePets.entities()[5].native &&
            nativePets.entities()[5].catType==3 && nativePets.entities()[6].native &&
            nativePets.entities()[6].wolfTame && nativePets.entities()[6].sitting &&
            nativePets.entities()[6].collarColor==11,
            "Native cat variant and tamed wolf state reach the renderer");
    World extraMobs;extraMobs.generate(89,true);
    constexpr std::array<int,7> newEggs{55,56,58,61,62,94,96};
    constexpr std::array<std::wstring_view,7> newIds{L"Slime",L"Ghast",L"Enderman",
        L"Blaze",L"LavaSlime",L"Squid",L"MushroomCow"};
    for(int i=0;i<int(newEggs.size());++i){
        const int x=65+i*10,z=95;
        const double y=extraMobs.surface(x,z)+1;
        require(extraMobs.spawnCreativeEgg(newEggs[i],{x+.5,y,z+.5}),
                "Remaining source egg creature can spawn with its own collision size");
        require(extraMobs.entities().back().id==newIds[i],
                "New spawn egg creates the source entity type");
    }
    const int firstSize=extraMobs.entities().front().slimeSize;
    const int magmaSize=extraMobs.entities()[4].slimeSize;
    require((firstSize==1 || firstSize==2 || firstSize==4) &&
            (magmaSize==1 || magmaSize==2 || magmaSize==4),
            "Slime eggs choose the source power-of-two size");
    const auto extraSave=std::filesystem::temp_directory_path()/
        ("console-creative-extra-"+std::to_string(getpid())+".inner");
    SaveCleanup extraSaveCleanup{extraSave};
    extraMobs.save(extraSave);
    World extraReload;
    require(extraReload.load(extraSave) && extraReload.entities().size()==newEggs.size() &&
            extraReload.entities()[0].slimeSize==firstSize &&
            extraReload.entities()[4].slimeSize==magmaSize,
            "New creature types and slime sizes survive native save/reload");
    World lowTunnel;lowTunnel.generate(88,true);
    const int tunnelY=lowTunnel.surface(80,64)+1;
    require(lowTunnel.set(80,tunnelY+1,64,Stone),"Low tunnel ceiling can be prepared");
    const Vec3 tunnelFeet{80.5,double(tunnelY),64.5};
    require(lowTunnel.collides(tunnelFeet) && !lowTunnel.collides(tunnelFeet,.3,.7) &&
            lowTunnel.spawnCreativeEgg(60,tunnelFeet) &&
            !lowTunnel.spawnCreativeEgg(50,tunnelFeet),
            "Silverfish fits the source-height tunnel while a player or Creeper does not");
    require(placement.placeBlock(70,200,0,static_cast<Block>(placedTileForItem(379)),0,{75,200,5},0) &&
            placement.canOpenBrewingStand(70,200,0) &&
            placement.placeBlock(71,200,0,static_cast<Block>(placedTileForItem(380)),0,{75,200,5},0) &&
            placement.get(71,200,0)==static_cast<Block>(118),
            "Source creative item IDs place usable brewing and cauldron blocks");
    require(creativeTab(6).items.front()==CreativeEntry{384,0} &&
            creativeTab(6).items.back()==CreativeEntry{373,24588} &&
            creativeTab(7).items.back()==CreativeEntry{383,120},
            "Brewing potion and miscellaneous spawn egg variants");
    bool rejected=false;try{creativeTab(8);}catch(const std::out_of_range&){rejected=true;}
    require(rejected,"Invalid creative tab is rejected");
    std::cout<<"creative catalog tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
