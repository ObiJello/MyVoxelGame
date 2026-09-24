#include "World.h"
#include "PS3WorldStorage.h"
#include "NbtIo.h"
#include "ByteArrayInputStream.h"
#include "ByteArrayOutputStream.h"
#include "DataInputStream.h"
#include "DataOutputStream.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::unique_ptr<CompoundTag> readPlayer(std::span<const unsigned char> bytes){
    ByteArrayInputStream input(byteArray(const_cast<unsigned char*>(bytes.data()),bytes.size()));
    DataInputStream stream(&input);
    std::unique_ptr<CompoundTag> player;
    try{player.reset(NbtIo::read(&stream));input.reset();}
    catch(...){input.reset();throw;}
    return player;
}
static void writePlayer(console::PS3WorldStorage& archive,CompoundTag& player){
    ByteArrayOutputStream bytes;DataOutputStream output(&bytes);
    NbtIo::write(&player,&output);
    archive.putEntry(L"console_port.inventory.dat",
        std::span<const unsigned char>(bytes.buf.data,bytes.size()));
}
int main(){try{
    using namespace console;
    const auto path=std::filesystem::temp_directory_path()/
        ("console-player-vitals-"+std::to_string(getpid())+".inner");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;
        std::filesystem::remove(path,error);}} cleanup{path};
    World world;world.generate(2026,true);world.save(path);
    auto archive=PS3WorldStorage::readFile(path);
    auto player=readPlayer(archive->entry(L"console_port.inventory.dat"));
    require(player->getInt(L"foodLevel")==20 &&
            player->getFloat(L"foodSaturationLevel")==5.f &&
            player->getInt(L"XpLevel")==0 && player->getInt(L"XpTotal")==0,
            "New worlds save the source player food and experience defaults");
    player->putShort(L"Health",12);
    player->putInt(L"foodLevel",20);
    player->putInt(L"foodTickTimer",79);
    player->putFloat(L"foodSaturationLevel",3.f);
    player->putFloat(L"foodExhaustionLevel",2.f);
    player->putFloat(L"XpP",.5f);
    player->putInt(L"XpLevel",4);
    player->putInt(L"XpTotal",45);
    player->putInt(L"UnportedPlayerField",883);
    writePlayer(*archive,*player);archive->writeFile(path);
    World loaded;
    require(loaded.load(path) && loaded.playerHealth()==12 &&
            loaded.playerFoodLevel()==20 && loaded.playerSaturation()==3.f &&
            loaded.playerExperienceLevel()==4 && loaded.playerTotalExperience()==45,
            "Player health, food, and XP load into the live world");
    loaded.tickTime();
    require(loaded.playerHealth()==13,
            "The live world runs the source eighty-tick food healing rule");
    loaded.save(path);
    archive=PS3WorldStorage::readFile(path);
    player=readPlayer(archive->entry(L"console_port.inventory.dat"));
    require(player->getInt(L"foodTickTimer")==0 &&
            player->getShort(L"Health")==13 &&
            player->getFloat(L"foodSaturationLevel")==3.f &&
            player->getFloat(L"foodExhaustionLevel")==2.f &&
            player->getFloat(L"XpP")==.5f &&
            player->getInt(L"XpLevel")==4 && player->getInt(L"XpTotal")==45 &&
            player->getInt(L"UnportedPlayerField")==883,
            "Ticked player state and unknown fields survive the native save path");
    auto legacy=PS3WorldStorage::readFile(path);
    for(const auto* key:{L"foodLevel",L"foodTickTimer",L"foodSaturationLevel",
                         L"foodExhaustionLevel",L"XpP",L"XpLevel",L"XpTotal"})
        player->remove(key);
    writePlayer(*legacy,*player);legacy->writeFile(path);
    World oldSave;
    require(oldSave.load(path) && oldSave.playerFoodLevel()==20 &&
            oldSave.playerSaturation()==5.f && oldSave.playerExperienceLevel()==0 &&
            oldSave.playerTotalExperience()==0 && oldSave.playerHealth()==13,
            "Older client saves without food or XP fields load with source defaults");
    archive=PS3WorldStorage::readFile(path);
    player->putInt(L"foodLevel",20);
    player->putFloat(L"foodSaturationLevel",99.f);
    writePlayer(*archive,*player);archive->writeFile(path);
    bool rejected=false;try{loaded.load(path);}catch(const std::exception&){rejected=true;}
    require(rejected && loaded.playerHealth()==13 && loaded.playerFoodLevel()==20 &&
            loaded.playerExperienceLevel()==4,
            "Invalid food state rejects the load without replacing the live world");
    std::cout<<"Live player food tick and XP/native-save integration passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
