#include "stdafx.h"
#include "RaycastFixture.h"
#include <bit>
#include <iostream>
int main(){
    Vec3::CreateNewThreadStorage();AABB::CreateNewThreadStorage();
    for(int origin:{-16,0,15})for(int flags=0;flags<4;++flags)for(int data:{0,8}){
        std::uint64_t hash=14695981039346656037ull;auto mix=[&](std::uint64_t value){hash=(hash^value)*1099511628211ull;};
        for(int i=0;i<10;++i){
            RaycastFixture access;access.cells[{origin+2,64,0}]={3,i%2};access.cells[{origin+3,64,0}]={4,0};access.cells[{origin+4,64,0}]={2,data};access.cells[{origin+5,64,0}]={1,0};
            console::BlockRaycaster ray(access);
            const double points[10][6]={{0,64.25,0.5,8,64.25,0.5},{0,64.75,0.5,8,64.75,0.5},{8,64.25,0.5,0,64.25,0.5},{4.5,68,0.5,4.5,60,0.5},{4.5,60,0.5,4.5,68,0.5},{4.5,64.25,-4,4.5,64.25,4},{4.5,64.75,4,4.5,64.75,-4},{-1,62,-2,8,66,2},{2.5,64.5,0.5,8,64.5,0.5},{0,80,0,400,80,0}};
            auto p=points[i];std::unique_ptr<Vec3> a(Vec3::newPermanent(origin+p[0],p[1],p[2])),b(Vec3::newPermanent(origin+p[3],p[4],p[5]));
            std::unique_ptr<HitResult> hit(ray.clip(a.get(),b.get(),flags&1,flags&2));mix(hit!=nullptr);
            if(hit){for(int value:{hit->x,hit->y,hit->z,hit->f})mix(value);for(double value:{hit->pos->x,hit->pos->y,hit->pos->z})mix(std::bit_cast<std::uint64_t>(value));}
            for(double value:{a->x,a->y,a->z})mix(std::bit_cast<std::uint64_t>(value));
            for(auto [x,y,z]:access.visits){mix(x);mix(y);mix(z);}
        }
        std::cout<<origin<<' '<<flags<<' '<<data<<' '<<hash<<'\n';
    }
}
