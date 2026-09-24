#include "PlayerExperience.h"
#include "NbtIo.h"
#include <bit>
#include <iostream>

int main() {
    int sample=0;
    for(int level : {0,1,14,15,16,29,30,31,100,1000})
    for(float progress : {0.f,.125f,.5f,.999f})
    for(int award : {0,1,16,17,62,100,121,1000,123456}) {
        CompoundTag tag; tag.putFloat(L"XpP",progress); tag.putInt(L"XpLevel",level); tag.putInt(L"XpTotal",123);
        console::PlayerExperience xp; xp.readAdditionalSaveData(&tag);
        std::uint64_t hash=14695981039346656037ull;
        auto mix=[&](std::uint64_t value){ hash=(hash^value)*1099511628211ull; };
        for(int step=0;step<12;++step) {
            xp.increaseXp(award);
            if(step%3==2)xp.withdrawExperienceLevels(step);
            if(step%4==3)xp.levelUp();
            xp.addAdditionalSaveData(&tag);
            mix(xp.getLevel());mix(xp.getTotal());mix(xp.getScore());mix(std::bit_cast<std::uint32_t>(xp.getProgress()));
            mix(xp.getXpNeededForNextLevel());mix(xp.getExperienceReward());mix(xp.isAlwaysExperienceDropper());
            ByteArrayOutputStream stream;DataOutputStream output(&stream);NbtIo::write(&tag,&output);
            for(unsigned i=0;i<stream.size();++i)mix(stream.buf[i]);
            if(step==7) { console::PlayerExperience loaded;loaded.readAdditionalSaveData(&tag);xp=loaded; }
        }
        std::cout<<sample++<<' '<<hash<<'\n';
    }
}
