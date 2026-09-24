#include "stdafx.h"
#include "Material.h"
#include <iostream>
int main(){
    MaterialColor::staticCtor();Material::staticCtor();
    Material* materials[]={Material::air,Material::grass,Material::dirt,Material::wood,
        Material::stone,Material::metal,Material::heavyMetal,Material::water,Material::lava,
        Material::leaves,Material::plant,Material::replaceable_plant,Material::sponge,
        Material::cloth,Material::fire,Material::sand,Material::decoration,
        Material::clothDecoration,Material::glass,Material::buildable_glass,
        Material::explosive,Material::coral,Material::ice,Material::topSnow,Material::snow,
        Material::cactus,Material::clay,Material::vegetable,Material::egg,Material::portal,
        Material::cake,Material::web,Material::piston};
    for(auto* m:materials){
        std::cout<<m->color->id<<' '<<m->color->col<<' '
            <<m->isLiquid()<<m->letsWaterThrough()<<m->isSolid()<<m->blocksLight()
            <<m->blocksMotion()<<m->isFlammable()<<m->isReplaceable()
            <<m->isSolidBlocking()<<m->isAlwaysDestroyable()<<m->getPushReaction()
            <<m->isDestroyedByHand()<<'\n';
    }
}
