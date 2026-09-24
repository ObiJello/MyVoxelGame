#include "PotionEffects.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace console {
namespace {
struct Formula {
    int id;
    std::string_view duration;
    std::string_view amplifier;
    double durationModifier;
    bool instant;
};
// PotionBrewing::staticCtor's _SIMPLIFIED_BREWING table and the corresponding
// MobEffect constructors' duration modifiers. Other effect ids have no potion
// formula in this console version.
constexpr std::array formulas{
    Formula{10,"0 & !1 & !2 & !3 & 0+6","5",.25,false},
    Formula{1,"!0 & 1 & !2 & !3 & 1+6","5",1.,false},
    Formula{12,"0 & 1 & !2 & !3 & 0+6","",1.,false},
    Formula{6,"0 & !1 & 2 & !3","5",1.,true},
    Formula{19,"!0 & !1 & 2 & !3 & 2+6","5",.25,false},
    Formula{18,"!0 & !1 & !2 & 3 & 3+6","",.5,false},
    Formula{7,"!0 & !1 & 2 & 3","5",.5,true},
    Formula{2,"!0 & 1 & !2 & 3 & 3+6","",.5,false},
    Formula{5,"0 & !1 & !2 & 3 & 3+6","5",1.,false},
    Formula{16,"!0 & 1 & 2 & !3 & 2+6","",1.,false},
    Formula{14,"!0 & 1 & 2 & 3 & 2+6","",1.,false},
};
int sumTerms(std::string_view expression,int bits){
    int result=0,bit=0;
    bool hasBit=false,notBit=false,negative=false;
    const auto flush=[&]{
        if(!hasBit)return;
        const int on=bit<0 || bit>14?0:int((bits&(1<<bit))!=0);
        result+=(notBit?1-on:on)*(negative?-1:1);
        bit=0;hasBit=notBit=negative=false;
    };
    for(char c:expression){
        if(c>='0' && c<='9'){bit=bit*10+c-'0';hasBit=true;}
        else if(c=='!' || c=='-' || c=='+'){
            flush();if(c=='!')notBit=true;else if(c=='-')negative=true;
        }
    }
    flush();return result;
}
int evaluate(std::string_view expression,int bits){
    // Source parseEffectFormulaValue interprets '&' as all-positive clauses,
    // returning the largest clause value (used as the duration tier).
    int largest=0;
    while(true){
        const auto end=expression.find('&');
        const int value=sumTerms(expression.substr(0,end),bits);
        if(value<=0)return 0;
        largest=std::max(largest,value);
        if(end==std::string_view::npos)return largest;
        expression.remove_prefix(end+1);
    }
}
}
std::vector<PotionEffect> potionEffects(int damage){
    std::vector<PotionEffect> effects;
    for(const auto& entry:formulas){
        const int tier=evaluate(entry.duration,damage);
        if(tier<=0)continue;
        const int amplifier=entry.amplifier.empty()?0:std::max(0,evaluate(entry.amplifier,damage));
        int duration=1;
        if(!entry.instant){
            duration=1200*(tier*3+(tier-1)*2);
            duration>>=amplifier;
            duration=static_cast<int>(std::round(duration*entry.durationModifier));
            if((damage&0x4000)!=0)
                duration=static_cast<int>(std::round(duration*.75+.5));
        }
        effects.push_back({entry.id,duration,amplifier});
    }
    return effects;
}
void addPotionEffects(std::vector<PotionEffect>& active,const std::vector<PotionEffect>& incoming){
    for(const auto& effect:incoming){
        if(effect.duration<=0)continue;
        auto previous=std::find_if(active.begin(),active.end(),[&](const PotionEffect& item){return item.id==effect.id;});
        if(previous==active.end())active.push_back(effect);
        else if(effect.amplifier>previous->amplifier)*previous=effect;
        else if(effect.amplifier==previous->amplifier && effect.duration>previous->duration)
            previous->duration=effect.duration;
    }
}
void tickPotionEffects(std::vector<PotionEffect>& active,int& health,bool invulnerable){
    for(auto& effect:active){
        if(effect.duration<=0)continue;
        const int interval=effect.amplifier>=5?0:25>>std::max(0,effect.amplifier);
        if(effect.id==10 && (interval==0 || effect.duration%interval==0))
            health=std::min(20,health+1);
        else if(effect.id==19 && !invulnerable && health>1 &&
                (interval==0 || effect.duration%interval==0))--health;
        else if(effect.id==6)
            health=std::min(20,health+std::min(20,6<<std::clamp(effect.amplifier,0,2)));
        else if(effect.id==7 && !invulnerable)
            health=std::max(0,health-std::min(20,6<<std::clamp(effect.amplifier,0,2)));
        --effect.duration;
    }
    active.erase(std::remove_if(active.begin(),active.end(),[](const PotionEffect& item){return item.duration<=0;}),active.end());
}
double potionMovementMultiplier(const std::vector<PotionEffect>& active){
    double multiplier=1.;
    for(const auto& effect:active){
        if(effect.duration<=0)continue;
        if(effect.id==1)multiplier*=1.+.2*(effect.amplifier+1);
        else if(effect.id==2)multiplier*=1.-.15*(effect.amplifier+1);
    }
    return std::max(0.,multiplier);
}
int potionEffectRemaining(const std::vector<PotionEffect>& active,int id){
    for(const auto& effect:active)if(effect.id==id)return std::max(0,effect.duration);
    return 0;
}
}
