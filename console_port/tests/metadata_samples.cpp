#include "LevelData.h"
#include "LevelSettings.h"
#include "LevelType.h"
#include "Abilities.h"
#include "NbtIo.h"
#include <iostream>
#include <memory>
static std::uint64_t tagHash(CompoundTag* tag){
    // Wall-clock time is a platform input, not a deterministic game field.
    if(tag->contains(L"LastPlayed"))tag->putLong(L"LastPlayed",123456789);
    ByteArrayOutputStream stream;DataOutputStream out(&stream);NbtIo::write(tag,&out);
    std::uint64_t value=14695981039346656037ull;for(unsigned i=0;i<stream.size();++i)value=(value^stream.buf[i])*1099511628211ull;return value;
}
int main(){
    LevelType::staticCtor();GameType::staticCtor();
    for(int mode:{-1,0,1,2,77})for(int size:{0,54,320}){
        LevelSettings settings(-9876543210ll,GameType::byId(mode),true,false,true,LevelType::lvl_normal,size,0);
        settings.enableStartingBonusItems()->enableSinglePlayerCommands();
        LevelData data(&settings,L"Console Save");
        data.setSpawn(-100,75,250);data.setTime(24001);data.setSizeOnDisk(876543);data.setVersion(19133);
        data.setRaining(true);data.setRainTime(80);data.setThundering(true);data.setThunderTime(25);data.setInitialized(true);
        data.setHasStronghold();data.setXStronghold(-256);data.setZStronghold(128);
        data.setHasStrongholdEndPortal();data.setXStrongholdEndPortal(-252);data.setZStrongholdEndPortal(130);
        std::unique_ptr<CompoundTag> tag(data.createTag());LevelData read(tag.get());LevelData copy(&read);
        std::unique_ptr<CompoundTag> copied(copy.createTag());
        Abilities abilities;data.getGameType()->updatePlayerAbilities(&abilities);abilities.setFlyingSpeed(0.075f);
        CompoundTag player;abilities.addSaveData(&player);Abilities decoded;decoded.loadSaveData(&player);
        std::cout<<mode<<' '<<size<<' '<<tagHash(tag.get())<<' '<<tagHash(copied.get())<<' '<<tagHash(&player)<<' '<<copy.getXZSize()<<' '<<copy.getHellScale()<<' '<<decoded.mayBuild<<' '<<decoded.mayfly<<'\n';
    }
    for(bool cheats:{false,true}){
        CompoundTag tag;tag.putInt(L"GameType",1);tag.putString(L"generatorName",L"default");tag.putInt(L"generatorVersion",0);
        LevelData legacy(&tag);SavePlatform::cheatsEnabled=cheats;legacy.setGameType(GameType::SURVIVAL);
        std::cout<<legacy.getGenerator()->getVersion()<<' '<<legacy.getAllowCommands()<<' '<<legacy.isInitialized()<<' '<<legacy.isGenerateMapFeatures()<<' '<<legacy.getHasBeenInCreative()<<'\n';
    }
}
