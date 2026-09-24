#include "food_fixture.h"
#include <iostream>
#include <limits>

static void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> static void rejects(F action) {
    try { action(); } catch(const std::exception&) { return; }
    throw std::runtime_error("Expected invalid food input to fail");
}
static int timer(FoodData& food) { CompoundTag tag; food.addAdditonalSaveData(&tag); return tag.getInt(L"foodTickTimer"); }
static void tick(FoodData& food,FoodFixture& player,int count) { for(int i=0;i<count;++i)player.tick(food); }
int main() { try {
    FoodData food; FoodFixture player;
    require(food.getFoodLevel()==20 && food.getLastFoodLevel()==20 && food.getSaturationLevel()==5 && food.getExhaustionLevel()==0 && !food.needsFood(),"Original initial food state");
    food.setFoodLevel(18); food.setSaturation(0); food.addExhaustion(4); player.tick(food);
    require(food.getFoodLevel()==18 && food.getExhaustionLevel()==4,"Exhaustion uses strict greater-than-four threshold");
    food.addExhaustion(.25f); player.tick(food);
    require(food.getFoodLevel()==17 && food.getLastFoodLevel()==18 && food.getExhaustionLevel()==.25f,"Drop one food after exhaustion threshold");
    food.setExhaustion(40); food.setSaturation(.25f); player.tick(food);
    require(food.getFoodLevel()==17 && food.getSaturationLevel()==0 && food.getExhaustionLevel()==36,"One exhaustion drop per tick, saturation consumed first");
    player.difficulty(0); player.tick(food); require(food.getFoodLevel()==17 && food.getExhaustionLevel()==32,"Peaceful consumes exhaustion without food loss");
    food.eat(100,1.2f); require(food.getFoodLevel()==20 && food.getSaturationLevel()==20,"Eating caps hunger and saturation");
    food.addExhaustion(1000); require(food.getExhaustionLevel()==40,"Exhaustion cap");

    food=FoodData(); player=FoodFixture(); player.health=15;
    tick(food,player,79); require(player.healed==0 && timer(food)==79,"Healing waits 80 ticks");
    player.tick(food); require(player.health==16 && player.healed==1 && timer(food)==0 && food.getExhaustionLevel()==0,"Original healing does not add modern exhaustion cost");
    food.setFoodLevel(17); tick(food,player,100); require(player.healed==1 && timer(food)==0,"Below heal threshold resets timer");
    for(int mode=0;mode<=3;++mode) {
        food=FoodData(); player=FoodFixture(); player.difficulty(mode); food.setFoodLevel(0); food.setSaturation(0);
        tick(food,player,80*22);
        require(player.health==(mode<2?10:mode==2?1:0),"Original starvation health floors by difficulty");
    }
    food=FoodData(); player=FoodFixture(); player.ignore=true; player.health=18; food.setFoodLevel(2);
    tick(food,player,79); player.health=20; tick(food,player,11);
    require(timer(food)==79,"Console hunger-ignore branch retains timer while no healing is needed");
    player.health=18; player.tick(food);
    require(player.health==19 && food.getFoodLevel()==1 && timer(food)==0,"Console hunger-ignore heals at the cost of one food");
    food.setFoodLevel(0); tick(food,player,160); require(player.starved==0,"Host hunger-ignore disables starvation");
    food.setExhaustion(5); food.setSaturation(0); food.setFoodLevel(2); player.tick(food);
    require(food.getFoodLevel()==1,"FoodData still drains existing exhaustion; player policy must suppress new exhaustion");

    food=FoodData(); player=FoodFixture(); player.health=10; tick(food,player,37); food.setExhaustion(1.25f);
    CompoundTag tag; tag.putString(L"opaquePlayerField",L"retain"); food.addAdditonalSaveData(&tag);
    auto bytes=NbtIo::compress(&tag); std::unique_ptr<unsigned char[]> owned(bytes.data);
    std::unique_ptr<CompoundTag> decoded(NbtIo::decompress(bytes)); FoodData loaded; loaded.readAdditionalSaveData(decoded.get());
    require(timer(loaded)==37 && loaded.getFoodLevel()==20 && loaded.getSaturationLevel()==5 && loaded.getExhaustionLevel()==1.25f && decoded->getString(L"opaquePlayerField")==L"retain","Food save wire round trip and opaque fields");
    tick(loaded,player,43); require(player.health==11,"Saved heal timer resumes after reload");
    CompoundTag empty; loaded.setFoodLevel(7); loaded.readAdditionalSaveData(&empty); require(loaded.getFoodLevel()==7,"Absent foodLevel retains previous state");
    empty.putInt(L"foodLevel",12); loaded.readAdditionalSaveData(&empty);
    require(loaded.getFoodLevel()==12 && loaded.getSaturationLevel()==0 && loaded.getExhaustionLevel()==0 && timer(loaded)==0,"Legacy missing fields retain original zero defaults");
    const auto before=loaded.getFoodLevel();
    for(float invalid : {-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        rejects([&]{ loaded.addExhaustion(invalid); }); rejects([&]{ loaded.setSaturation(invalid); });
        rejects([&]{ loaded.setExhaustion(invalid); }); rejects([&]{ loaded.eat(1,invalid); });
        tag.putFloat(L"foodExhaustionLevel",invalid); rejects([&]{loaded.readAdditionalSaveData(&tag);});
        require(loaded.getFoodLevel()==before && loaded.getSaturationLevel()==0,"Malformed save load is transactional");
    }
    food.addAdditonalSaveData(&tag); tag.putInt(L"foodTickTimer",80); rejects([&]{loaded.readAdditionalSaveData(&tag);});
    tag.putInt(L"foodTickTimer",0); tag.putLong(L"foodLevel",20); rejects([&]{loaded.readAdditionalSaveData(&tag);});
    rejects([&]{loaded.readAdditionalSaveData(nullptr);}); rejects([&]{loaded.addAdditonalSaveData(nullptr);});
    rejects([&]{loaded.setFoodLevel(-1);}); rejects([&]{loaded.setFoodLevel(21);}); rejects([&]{loaded.eat(-1,0);});
    rejects([&]{loaded.setSaturation(21);}); rejects([&]{loaded.setExhaustion(41);});
    player.difficulty(4); rejects([&]{player.tick(loaded);});
    loaded.eat(INT_MAX, std::numeric_limits<float>::max()); require(loaded.getFoodLevel()==20 && loaded.getSaturationLevel()==20,"Large nutrition must not overflow signed food addition");
    std::cout<<"Food, exhaustion, host hunger option, healing/starvation and save checks passed\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; } }
