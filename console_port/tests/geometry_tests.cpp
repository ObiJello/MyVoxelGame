#include "stdafx.h"
#include "Vec3.h"
#include "AABB.h"
#include "HitResult.h"
#include <future>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    // Native callers may use pools before explicit thread setup.
    auto* a=Vec3::newTemp(3,4,0);require(a->length()==5,"Vector length");auto* normal=a->normalize();require(normal->x==0.6 && normal->y==0.8,"Vector normalization retains source arithmetic");
    require(Vec3::newTemp(0.000001,0,0)->normalize()->length()==0,"Original near-zero normalization threshold");
    auto* cross=Vec3::newTemp(1,0,0)->cross(Vec3::newTemp(0,1,0));require(cross->x==0 && cross->y==0 && cross->z==1,"Right-handed cross product");
    std::unique_ptr<Vec3> permanent(Vec3::newPermanent(-0.0,-0.0,-0.0));require(!std::signbit(permanent->x) && std::signbit(Vec3::newTemp(-0.0,0,0)->x),"Original permanent/temp signed-zero behavior");
    std::unique_ptr<AABB> cube(AABB::newPermanent(0,0,0,1,1,1));
    require(!cube->contains(Vec3::newTemp(0,0.5,0.5)) && cube->containsIncludingLowerBound(Vec3::newTemp(0,0.5,0.5)) && !cube->containsIncludingLowerBound(Vec3::newTemp(1,0.5,0.5)),"Strict and lower-inclusive containment boundaries");
    auto* adjacent=AABB::newTemp(1,0,0,2,1,1);require(!cube->intersects(adjacent) && cube->intersectsInner(adjacent),"Touching boxes have the original distinct overlap semantics");
    require(cube->clipXCollide(AABB::newTemp(-2,0,0,-1,1,1),5)==1 && cube->clipXCollide(AABB::newTemp(2,0,0,3,1,1),-5)==-1,"X movement stops at either face");
    require(cube->clipYCollide(AABB::newTemp(0,-2,0,1,-1,1),5)==1 && cube->clipZCollide(AABB::newTemp(0,0,2,1,1,3),-5)==-1,"Y/Z collision axis handling");
    require(cube->clipXCollide(AABB::newTemp(-2,1,0,-1,2,1),5)==5,"Movement past a tangent face remains unobstructed");
    const double starts[6][3]={{0.5,-1,0.5},{0.5,2,0.5},{0.5,0.5,-1},{0.5,0.5,2},{-1,0.5,0.5},{2,0.5,0.5}};
    for(int face=0;face<6;++face){auto p=starts[face];std::unique_ptr<HitResult> hit(cube->clip(Vec3::newTemp(p[0],p[1],p[2]),Vec3::newTemp(0.5,0.5,0.5)));require(hit && hit->type==HitResult::TILE && hit->f==face && !hit->entity,"Ray clip returns the original face ID");}
    // An exact binary intersection isolates tie ordering from boundary rounding.
    std::unique_ptr<HitResult> corner(cube->clip(Vec3::newTemp(-1,-1,-1),Vec3::newTemp(1,1,1)));require(corner && corner->f==4,"Equal-distance corner hits keep the original X-first tie order");
    std::unique_ptr<HitResult> inside(cube->clip(Vec3::newTemp(0.5,0.5,0.5),Vec3::newTemp(2,0.5,0.5)));require(inside && inside->f==5,"Ray starting inside hits the exit face");
    std::unique_ptr<HitResult> miss(cube->clip(Vec3::newTemp(-1,2,0.5),Vec3::newTemp(2,2,0.5)));require(!miss,"Ray outside the box misses");
    require(!Vec3::newTemp(0,0,0)->clipX(Vec3::newTemp(0.0001,1,1),0.00005),"Original near-parallel clip tolerance");
    require(Vec3::newTemp(2,2,2)->distanceTo(cube.get())==std::sqrt(3.0),"Point-to-box corner distance");
    // Ring reuse is an intentional original API contract, not stable ownership.
    Vec3::ReleaseThreadStorage();auto* first=Vec3::newTemp(91,92,93);for(int i=0;i<1023;++i)Vec3::newTemp(i,0,0);require(first->x==91,"Temporary vectors survive until their pool slot is reused");require(Vec3::newTemp(7,8,9)==first && first->x==7,"Original 1024-entry vector ring reuse");
    AABB::ReleaseThreadStorage();auto* firstBox=AABB::newTemp(91,0,0,92,1,1);for(int i=0;i<1023;++i)AABB::newTemp(i,0,0,i+1,1,1);require(firstBox->x0==91,"Temporary boxes survive until slot reuse");require(AABB::newTemp(7,0,0,8,1,1)==firstBox,"Original 1024-entry box ring reuse");
    auto* mainVector=Vec3::newTemp(123,456,789);auto* mainBox=AABB::newTemp(123,0,0,124,1,1);
    std::vector<std::future<void>> workers;
    for(int worker=0;worker<8;++worker)workers.push_back(std::async(std::launch::async,[worker]{
        Vec3::UseDefaultThreadStorage();AABB::UseDefaultThreadStorage();
        for(int i=0;i<3000;++i){auto* v=Vec3::newTemp(worker,i,1);auto* b=AABB::newTemp(worker,0,0,worker+1,1,1);require(v->x==worker && b->x0==worker,"Worker pools must be independent");require(!v->toString().empty(),"Concurrent diagnostic formatting");}
        Vec3::ReleaseThreadStorage();Vec3::ReleaseThreadStorage();AABB::ReleaseThreadStorage();AABB::ReleaseThreadStorage();require(Vec3::newTemp(1,2,3)->z==3 && AABB::newTemp(0,0,0,1,1,1)->x1==1,"Pools recreate lazily after repeated release");
    }));
    for(auto& worker:workers)worker.get();require(mainVector->x==123 && mainBox->x0==123 && permanent->x==0,"Worker allocation leaves main-thread pools and permanent objects intact");
    std::cout<<"Original vector geometry, collision boundaries, six ray faces, pool lifecycle and thread isolation passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
