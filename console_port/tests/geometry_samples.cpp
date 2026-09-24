#include "stdafx.h"
#include "Vec3.h"
#include "AABB.h"
#include "HitResult.h"
#include <bit>
#include <iostream>
static std::uint64_t fingerprint=14695981039346656037ull;
static void mix(std::uint64_t value){fingerprint=(fingerprint^value)*1099511628211ull;}
static void scalar(double value){mix(std::bit_cast<std::uint64_t>(value));}
static void captureVector(Vec3* value){mix(value!=nullptr);if(value){scalar(value->x);scalar(value->y);scalar(value->z);}}
static void box(AABB* value){scalar(value->x0);scalar(value->y0);scalar(value->z0);scalar(value->x1);scalar(value->y1);scalar(value->z1);}
int main(){
    Vec3::CreateNewThreadStorage();AABB::CreateNewThreadStorage();
    for(int origin:{-432,-1,0,431})for(double size:{0.125,1.0,17.0}){
        fingerprint=14695981039346656037ull;
        for(int i=0;i<32;++i){
            std::unique_ptr<Vec3> a(Vec3::newPermanent(origin+i*0.125,i*0.25-4,origin-i*0.5)),b(Vec3::newPermanent(origin-i*0.5,7-i*0.0625,origin+i*0.25));
            std::unique_ptr<AABB> bounds(AABB::newPermanent(origin,-2,origin,origin+size,size,origin+size));
            captureVector(a->interpolateTo(b.get(),0.25));captureVector(a->lerp(b.get(),0.75));captureVector(a->vectorTo(b.get()));captureVector(a->normalize());scalar(a->dot(b.get()));captureVector(a->cross(b.get()));
            captureVector(a->add(size,-size,0));scalar(a->distanceTo(b.get()));scalar(a->distanceToSqr(b.get()));scalar(a->distanceToSqr(b->x,b->y,b->z));captureVector(a->scale(-0.5));scalar(a->length());scalar(a->distanceTo(bounds.get()));
            captureVector(a->clipX(b.get(),origin));captureVector(a->clipY(b.get(),0));captureVector(a->clipZ(b.get(),origin));
            a->xRot(float(i)*0.125f);captureVector(a.get());a->yRot(float(i)*-0.125f);captureVector(a.get());a->zRot(float(i)*0.25f);captureVector(a.get());
            auto* expanded=bounds->expand(-size,size,0.5);box(expanded);box(bounds->grow(0.5,1,0.25));box(bounds->shrink(0.125,0.25,0.125));box(bounds->cloneMove(-1,0,2));
            auto* moved=bounds->copy()->move(i*0.125-2,0.25,0.125);
            for(double delta:{-4.0,0.0,4.0}){scalar(bounds->clipXCollide(moved,delta));scalar(bounds->clipYCollide(moved,delta));scalar(bounds->clipZCollide(moved,delta));}
            mix(bounds->intersects(moved));mix(bounds->intersectsInner(moved));mix(bounds->intersects(moved->x0,moved->y0,moved->z0,moved->x1,moved->y1,moved->z1));
            mix(bounds->contains(a.get()));mix(bounds->containsIncludingLowerBound(a.get()));scalar(bounds->getSize());
            std::unique_ptr<HitResult> hit(bounds->clip(a.get(),b.get()));mix(hit!=nullptr);if(hit){mix(hit->type);mix(hit->f);captureVector(hit->pos);}
            auto* copied=AABB::newTemp(0,0,0,0,0,0);copied->set(bounds.get());box(copied);
            for(auto ch:bounds->toString())mix(ch);for(auto ch:a->toString())mix(ch);
        }
        std::cout<<origin<<' '<<size<<' '<<fingerprint<<'\n';
    }
}
