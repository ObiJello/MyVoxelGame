#include "LevelData.h"
#include "LevelSettings.h"
#include "LevelType.h"
#include "Abilities.h"
#include "NbtIo.h"
#include "System.h"
#include <iostream>
#include <thread>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection");}
int main(){try{
    LevelType::staticCtor();GameType::staticCtor();auto* normal=LevelType::lvl_normal;auto* survival=GameType::SURVIVAL;
    std::vector<std::thread> workers;for(int i=0;i<8;++i)workers.emplace_back([]{for(int j=0;j<100;++j){LevelType::staticCtor();GameType::staticCtor();}});for(auto& worker:workers)worker.join();
    require(LevelType::lvl_normal==normal && GameType::SURVIVAL==survival,"Registry initialization must preserve existing pointers");
    require(GameType::byId(99)==survival && GameType::byName(L"unknown")==survival,"Unknown game modes fall back to survival");
    CompoundTag old;LevelData legacy(&old);require(legacy.isGenerateMapFeatures() && legacy.isInitialized() && !legacy.getAllowCommands(),"Legacy metadata defaults");
    require(legacy.getXZSize()==54 && legacy.getHellScale()==3,"PS3 dimensions clamp to original finite world sizes");
    old.putString(L"generatorName",L"missing generator");LevelData unknown(&old);require(unknown.getGenerator()==normal,"Unknown generator fallback must avoid empty registry slots");
    old.putString(L"generatorName",L"default");old.putInt(L"generatorVersion",0);LevelData previous(&old);require(previous.getGenerator()==LevelType::lvl_normal_1_1,"Version-zero default must select legacy generator");
    old.putString(L"generatorName",L"default_1_1");LevelData explicitPrevious(&old);require(explicitPrevious.getGenerator()==LevelType::lvl_normal_1_1,"Legacy generator name must load through registry gaps");
    old.putString(L"generatorName",L"flat");LevelData flat(&old);require(flat.getGenerator()==LevelType::lvl_flat,"Flat generator replacement flag is initialized");
    old.putInt(L"GameType",1);LevelData creativeOld(&old);require(creativeOld.getAllowCommands(),"Legacy creative command default");
    old.putBoolean(L"allowCommands",false);LevelData creativeExplicit(&old);require(!creativeExplicit.getAllowCommands(),"Explicit command flag overrides creative default");
    LevelSettings settings(INT64_MIN,GameType::SURVIVAL,false,true,true,normal,INT_MAX,INT_MIN);
    settings.enableStartingBonusItems()->enableSinglePlayerCommands();LevelData data(&settings,L"World \U0001f30d");
    require(data.getTime()==-1 && !data.isInitialized() && data.getSpawnBonusChest(),"New-world sentinel and bonus chest settings");
    data.setSpawn(-432,70,431);data.setHasStronghold();data.setXStronghold(-123);data.setZStronghold(456);data.setHasStrongholdEndPortal();data.setXStrongholdEndPortal(-125);data.setZStrongholdEndPortal(458);
    data.setTime(INT64_MAX);data.setRaining(true);data.setRainTime(120);data.setThundering(true);data.setThunderTime(30);data.setSizeOnDisk(1000);data.setVersion(19133);data.setInitialized(true);
    const auto before=System::currentTimeMillis();std::unique_ptr<CompoundTag> tag(data.createTag());const auto after=System::currentTimeMillis();require(tag->getLong(L"LastPlayed")>=before && tag->getLong(L"LastPlayed")<=after,"Save timestamp must use host epoch milliseconds");
    auto bytes=NbtIo::compress(tag.get());std::unique_ptr<unsigned char[]> owned(bytes.data);std::unique_ptr<CompoundTag> decoded(NbtIo::decompress(bytes));LevelData read(decoded.get());
    require(read.getSeed()==INT64_MIN && read.getTime()==INT64_MAX && read.getLevelName()==data.getLevelName(),"World seed, time and Unicode name round trip");
    require(read.getXSpawn()==-432 && read.getYSpawn()==70 && read.getZSpawn()==431,"Spawn round trip");
    require(read.getHasStronghold() && read.getXStronghold()==-123 && read.getZStronghold()==456 && read.getHasStrongholdEndPortal() && read.getXStrongholdEndPortal()==-125,"Structure metadata round trip");
    require(read.isRaining() && read.getRainTime()==120 && read.isThundering() && read.getThunderTime()==30,"Weather metadata round trip");
    require(read.getSizeOnDisk()==1000 && read.getVersion()==19133 && read.isInitialized() && read.isHardcore() && !read.isGenerateMapFeatures(),"World flags round trip");
    LevelData copied(&read);read.setLevelName(L"changed");require(copied.getLevelName()!=read.getLevelName(),"Level metadata copy must own its strings");
    SavePlatform::cheatsEnabled=false;data.setGameType(GameType::CREATIVE);data.setGameType(GameType::SURVIVAL);require(data.getHasBeenInCreative(),"Creative history must remain sticky");
    data.setHasBeenInCreative(false);SavePlatform::cheatsEnabled=true;data.setGameType(GameType::SURVIVAL);require(data.getHasBeenInCreative(),"Host cheats must mark creative history");SavePlatform::cheatsEnabled=false;
    Abilities abilities;GameType::CREATIVE->updatePlayerAbilities(&abilities);abilities.flying=true;require(abilities.mayfly && abilities.instabuild && abilities.invulnerable && abilities.mayBuild,"Creative abilities");
    GameType::ADVENTURE->updatePlayerAbilities(&abilities);require(!abilities.mayfly && !abilities.flying && !abilities.instabuild && !abilities.invulnerable && !abilities.mayBuild,"Adventure revokes flight and building");
    GameType::SURVIVAL->updatePlayerAbilities(&abilities);require(abilities.mayBuild && abilities.getFlyingSpeed()==0.05f && abilities.getWalkingSpeed()==0.1f,"Survival and default movement speeds");
    abilities.setFlyingSpeed(0.2f);abilities.setWalkingSpeed(0.3f);CompoundTag player;abilities.addSaveData(&player);Abilities loaded;loaded.loadSaveData(&player);require(loaded.getFlyingSpeed()==0.2f && loaded.getWalkingSpeed()==0.3f && loaded.mayBuild,"Abilities serialization");
    CompoundTag oldPlayer;auto* legacyAbilities=new CompoundTag;legacyAbilities->putBoolean(L"flying",true);oldPlayer.putCompound(L"abilities",legacyAbilities);Abilities defaults;defaults.loadSaveData(&oldPlayer);require(defaults.flying && defaults.mayBuild && defaults.getFlyingSpeed()==0.05f,"Legacy ability field defaults");
    rejects([&]{data.setGameType(nullptr);});rejects([&]{data.setGenerator(nullptr);});rejects([&]{LevelData invalid(static_cast<CompoundTag*>(nullptr));});
    std::cout<<"World metadata, PS3 limits, legacy defaults and player abilities passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
