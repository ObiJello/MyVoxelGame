#include "PlayerExperience.h"
#include "NbtIo.h"
#include <bit>
#include <iostream>
#include <limits>

static void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> static void rejects(F action) { try { action(); }catch(const std::exception&) { return; }throw std::runtime_error("Expected invalid XP state to fail"); }
static console::PlayerExperience make(int level,float progress=0,int total=0) {
    CompoundTag tag;tag.putFloat(L"XpP",progress);tag.putInt(L"XpLevel",level);tag.putInt(L"XpTotal",total);
    console::PlayerExperience xp;xp.readAdditionalSaveData(&tag);return xp;
}
int main() { try {
    using console::PlayerExperience;
    PlayerExperience xp;
    require(xp.getLevel()==0 && xp.getTotal()==0 && xp.getProgress()==0 && xp.getScore()==0,"Original initial XP state");
    for(auto [level,cost] : {std::pair{0,17},{14,17},{15,17},{16,20},{29,59},{30,62},{31,69},{100,552}}) {
        xp=make(level);require(xp.getXpNeededForNextLevel()==cost,"Original piecewise XP level costs");
    }
    xp=make(0);xp.increaseXp(17);require(xp.getLevel()==1 && xp.getProgress()==0 && xp.getTotal()==17 && xp.getScore()==17,"Exact level threshold");
    xp=make(14);xp.increaseXp(34);require(xp.getLevel()==16 && xp.getProgress()==0,"Cross original 15-level cost boundary");
    xp=make(29);xp.increaseXp(59);xp.increaseXp(62);require(xp.getLevel()==31 && xp.getProgress()==0,"Cross original 30-level cost boundary with exact individual awards");
    xp=make(29);xp.increaseXp(121);require(xp.getLevel()==31 && std::bit_cast<std::uint32_t>(xp.getProgress())==0x33e6076cu,"Retain original batch-award float rounding just above level 31");
    xp=make(40,.5f,12345);xp.withdrawExperienceLevels(3);
    require(xp.getLevel()==37 && xp.getProgress()==.5f && xp.getTotal()==12345,"Enchanting level withdrawal preserves progress and total");
    xp.withdrawExperienceLevels(INT_MAX);require(xp.getLevel()==0 && xp.getProgress()==.5f && xp.getTotal()==12345,"Withdrawal clamps only level to zero");
    for(auto [level,reward] : {std::pair{0,0},{1,7},{14,98},{15,100},{100,100},{PlayerExperience::maxLevel,100}}) {
        xp=make(level);require(xp.getExperienceReward()==reward && xp.isAlwaysExperienceDropper(),"Original death reward cap without overflow");
    }
    xp=make(1,.125f,INT_MAX-2);xp.increaseXp(10);
    require(xp.getTotal()==INT_MAX && xp.getScore()==10,"Total XP caps while score receives the original whole award");
    const auto progress=xp.getProgress();xp.increaseXp(INT_MAX);
    require(xp.getProgress()==progress && xp.getScore()==std::bit_cast<int>(std::uint32_t(10)+std::uint32_t(INT_MAX)),"Capped XP and defined original score wrapping");
    xp=make(0);xp.increaseXp(INT_MAX);require(xp.getTotal()==INT_MAX && xp.getLevel()>10000 && xp.getProgress()>=0 && xp.getProgress()<1,"Largest console XP award remains bounded and finite");
    xp=make(42,.375f,54321);xp.increaseXp(1);CompoundTag tag;tag.putString(L"otherPlayerData",L"retain");xp.addAdditionalSaveData(&tag);
    auto bytes=NbtIo::compress(&tag);std::unique_ptr<unsigned char[]> owned(bytes.data);std::unique_ptr<CompoundTag> decoded(NbtIo::decompress(bytes));
    PlayerExperience loaded;loaded.readAdditionalSaveData(decoded.get());
    require(loaded.getLevel()==xp.getLevel() && loaded.getTotal()==xp.getTotal() && loaded.getProgress()==xp.getProgress() && loaded.getScore()==0 && decoded->getString(L"otherPlayerData")==L"retain","Original XP save fields round trip, score is not an XP save field");
    loaded.increaseXp(1);CompoundTag old;loaded.readAdditionalSaveData(&old);
    require(loaded.getLevel()==0 && loaded.getTotal()==0 && loaded.getProgress()==0 && loaded.getScore()==1,"Legacy absent XP fields default to zero without resetting score");
    loaded=make(10,.5f,100);const auto before=loaded;
    for(float invalid : {-1.f,1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        tag.putFloat(L"XpP",invalid);rejects([&]{loaded.readAdditionalSaveData(&tag);});
        require(loaded.getLevel()==before.getLevel() && loaded.getProgress()==before.getProgress(),"Invalid XP load leaves previous state intact");
    }
    xp.addAdditionalSaveData(&tag);tag.putLong(L"XpLevel",1);rejects([&]{loaded.readAdditionalSaveData(&tag);});
    tag.putInt(L"XpLevel",-1);rejects([&]{loaded.readAdditionalSaveData(&tag);});
    tag.putInt(L"XpLevel",PlayerExperience::maxLevel+1);rejects([&]{loaded.readAdditionalSaveData(&tag);});
    tag.putInt(L"XpLevel",1);tag.putInt(L"XpTotal",-1);rejects([&]{loaded.readAdditionalSaveData(&tag);});
    rejects([&]{loaded.increaseXp(-1);});rejects([&]{loaded.withdrawExperienceLevels(-1);});
    rejects([&]{loaded.readAdditionalSaveData(nullptr);});rejects([&]{loaded.addAdditionalSaveData(nullptr);});
    loaded=make(PlayerExperience::maxLevel);rejects([&]{loaded.levelUp();});
    loaded=make(PlayerExperience::maxLevel,.5f);rejects([&]{loaded.increaseXp(INT_MAX);});
    require(loaded.getLevel()==PlayerExperience::maxLevel && loaded.getTotal()==0 && loaded.getScore()==0 && loaded.getProgress()==.5f,"Failed XP award is transactional");
    std::cout<<"Player XP thresholds, score, level withdrawal, death rewards and saves passed\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; } }
