#include "food_fixture.h"
#include <bit>
#include <iostream>

static void mix(std::uint64_t& hash, std::uint64_t value) {
    for(int i=0;i<8;++i) { hash = (hash ^ (value & 255)) * 1099511628211ull; value >>= 8; }
}
int main() {
    int sample = 0;
    for(int difficulty : {0,1,2,3}) for(bool ignore : {false,true})
    for(int foodLevel : {0,1,17,18,20}) for(float exhaustion : {0.f,4.f,40.f})
    for(float health : {0.f,1.f,10.f,11.f,19.f,20.f}) {
        FoodData food; FoodFixture fixture;
        fixture.difficulty(difficulty); fixture.state().ignore=ignore; fixture.state().health=health;
        food.setFoodLevel(foodLevel); food.setSaturation(sample%2 ? 0.f : 5.f); food.setExhaustion(exhaustion);
        std::uint64_t hash=14695981039346656037ull;
        for(int tick=0;tick<480;++tick) {
            if(tick==101) fixture.eatItem(food);
            if(tick==212) food.addExhaustion(FoodConstants::EXHAUSTION_SPRINT_JUMP);
            if(tick==293) fixture.state().ignore=!fixture.state().ignore;
            fixture.tick(food);
            mix(hash,food.getFoodLevel()); mix(hash,food.getLastFoodLevel());
            mix(hash,std::bit_cast<std::uint32_t>(food.getSaturationLevel()));
            mix(hash,std::bit_cast<std::uint32_t>(food.getExhaustionLevel()));
            mix(hash,std::bit_cast<std::uint32_t>(fixture.state().health));
            mix(hash,fixture.state().healed); mix(hash,fixture.state().starved);
            if(tick==37 || tick==317) {
                CompoundTag tag; food.addAdditonalSaveData(&tag);
                ByteArrayOutputStream stream; DataOutputStream output(&stream); NbtIo::write(&tag,&output);
                for(unsigned i=0;i<stream.size();++i)mix(hash,stream.buf[i]);
                FoodData loaded; loaded.readAdditionalSaveData(&tag); food=loaded;
            }
        }
        std::cout<<sample++<<' '<<hash<<'\n';
    }
}
