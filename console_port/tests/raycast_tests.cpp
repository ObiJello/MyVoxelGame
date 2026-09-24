#include "stdafx.h"
#include "RaycastFixture.h"
#include <iostream>
#include <limits>
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(){try{
    RaycastFixture access;console::BlockRaycaster ray(access);
    auto cast=[&](double y,bool liquid=false,bool solid=false){std::unique_ptr<Vec3> a(Vec3::newPermanent(-2,y,0.5)),b(Vec3::newPermanent(8,y,0.5));return std::unique_ptr<HitResult>(ray.clip(a.get(),b.get(),liquid,solid));};
    access.cells[{0,64,0}]={3,0};access.cells[{1,64,0}]={4,0};access.cells[{2,64,0}]={2,0};access.cells[{3,64,0}]={1,0};
    auto hit=cast(64.25);require(hit && hit->x==1 && hit->f==4,"Default selection skips liquids and can select non-colliding blocks");
    hit=cast(64.25,true);require(hit && hit->x==0,"Liquid picking selects a source block");access.cells[{0,64,0}].second=1;hit=cast(64.25,true);require(hit && hit->x==1,"Flowing liquid metadata is not source-pickable");
    hit=cast(64.25,false,true);require(hit && hit->x==2,"Solid-only selection skips non-colliding shapes");
    hit=cast(64.75,false,true);require(hit && hit->x==3,"Ray passes above a lower slab");access.cells[{2,64,0}].second=8;hit=cast(64.75,false,true);require(hit && hit->x==2,"Metadata-dependent upper shape participates in selection");
    access.cells.clear();access.cells[{-2,64,0}]={1,0};hit=cast(64.5);require(hit && hit->x==-2 && hit->f==4,"Original traversal considers the starting tile");
    access.cells.clear();access.visits.clear();std::unique_ptr<Vec3> a(Vec3::newPermanent(0,0,0)),b(Vec3::newPermanent(500,0,0));require(!ray.clip(a.get(),b.get()) && access.visits.size()==202,"Original traversal keeps its 201-step limit plus starting cell");
    auto visits=access.visits.size();a->x=std::numeric_limits<double>::infinity();require(!ray.clip(a.get(),b.get()) && access.visits.size()==visits,"Infinite coordinates are rejected before integer conversion");a->x=std::numeric_limits<double>::quiet_NaN();require(!ray.clip(a.get(),b.get()),"NaN coordinates cannot enter traversal");a->x=double(INT_MAX);require(!ray.clip(a.get(),b.get()),"Out-of-range coordinates cannot overflow cell conversion");require(!ray.clip(nullptr,b.get()),"Null ray endpoint is rejected");
    a->x=0;b->x=1;access.cells[{0,0,0}]={255,0};bool rejected=false;try{std::unique_ptr<HitResult> unused(ray.clip(a.get(),b.get()));}catch(const std::logic_error&){rejected=true;}require(rejected,"Unknown tile registry entries are not silently treated as empty");
    std::cout<<"Original ray traversal, source-liquid filtering, solid-only shapes, metadata, bounds and invalid input checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
