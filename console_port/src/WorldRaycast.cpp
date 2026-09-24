#include "World.h"
#include "BlockRaycaster.h"
#include "BlockShape.h"
#include "Facing.h"
#include <cmath>
namespace console {
namespace {
// Full-cube fallback for the remaining client blocks.
class ClientCube:public BoxRaycastTile {
    bool fluid;
    AABB* shape(BlockRaycaster*,int,int,int)override{return AABB::newTemp(0,0,0,1,1,1);}
public:
    explicit ClientCube(bool value):fluid(value){}
    AABB* getAABB(BlockRaycaster*,int x,int y,int z)override{return fluid?nullptr:AABB::newTemp(x,y,z,x+1,y+1,z+1);}
    bool mayPick(int data,bool liquid)override{return !fluid || (liquid && data==0);}
};
// StairTile::clip and updateShape use six occupied octants, even for
// corner stairs. Preserve that original picking behavior independently of the
// neighbor-aware collision boxes. Own discarded HitResults instead of leaking.
class ClientStair:public BoxRaycastTile {
    int clipStep=0;
    AABB* shape(BlockRaycaster*,int,int,int)override{
        double x=.5*(clipStep%2),y=.5*(clipStep/2%2),z=.5*(clipStep/4%2);
        return AABB::newTemp(x,y,z,x+.5,y+.5,z+.5);
    }
public:
    AABB* getAABB(BlockRaycaster*,int x,int y,int z)override{return AABB::newTemp(x,y,z,x+1,y+1,z+1);}
    bool mayPick(int,bool)override{return true;}
    HitResult* clip(BlockRaycaster* level,int x,int y,int z,::Vec3* a,::Vec3* b)override{
        static constexpr int dead[8][2]={{2,6},{3,7},{2,3},{6,7},{0,4},{1,5},{0,1},{4,5}};
        const int data=level->getData(x,y,z)&7;
        std::unique_ptr<HitResult> closest;double distance=0;
        for(clipStep=0;clipStep<8;++clipStep){
            if(clipStep==dead[data][0] || clipStep==dead[data][1])continue;
            std::unique_ptr<HitResult> hit(BoxRaycastTile::clip(level,x,y,z,a,b));
            if(hit && hit->pos->distanceToSqr(b)>distance){
                distance=hit->pos->distanceToSqr(b);closest=std::move(hit);
            }
        }
        return closest.release();
    }
};
class ClientDoor:public BoxRaycastTile {
    struct Access:BlockShapeAccess {
        BlockRaycaster& ray;explicit Access(BlockRaycaster& r):ray(r){}
        int getTile(int x,int y,int z)const override{return ray.getTile(x,y,z);}
        int getData(int x,int y,int z)const override{return ray.getData(x,y,z);}
    };
    BlockBox current{};
    AABB* shape(BlockRaycaster*,int,int,int)override{
        const auto& b=current;
        return AABB::newTemp(b.x0,b.y0,b.z0,b.x1,b.y1,b.z1);
    }
public:
    AABB* getAABB(BlockRaycaster* ray,int x,int y,int z)override{
        Access access(*ray);const auto shape=consoleCollisionShape(access,x,y,z);
        if(shape.count==0)return nullptr;
        auto b=shape.boxes[0];
        return AABB::newTemp(x+b.x0,y+b.y0,z+b.z0,x+b.x1,y+b.y1,z+b.z1);
    }
    bool mayPick(int,bool)override{return true;}
    HitResult* clip(BlockRaycaster* ray,int x,int y,int z,::Vec3* a,::Vec3* b)override{
        Access access(*ray);const auto boxes=consoleRenderShape(access,x,y,z);
        std::unique_ptr<HitResult> closest;double distance=0;
        for(std::size_t i=0;i<boxes.count;++i){
            current=boxes.boxes[i];
            std::unique_ptr<HitResult> hit(BoxRaycastTile::clip(ray,x,y,z,a,b));
            if(hit && hit->pos->distanceToSqr(b)>distance){
                distance=hit->pos->distanceToSqr(b);closest=std::move(hit);
            }
        }
        return closest.release();
    }
};
class ClientSlab:public BoxRaycastTile {
    AABB* shape(BlockRaycaster* level,int x,int y,int z)override{
        return level->getData(x,y,z)&8?AABB::newTemp(0,.5,0,1,1,1):AABB::newTemp(0,0,0,1,.5,1);
    }
public:
    AABB* getAABB(BlockRaycaster* level,int x,int y,int z)override{
        return level->getData(x,y,z)&8?AABB::newTemp(x,y+.5,z,x+1,y+1,z+1):AABB::newTemp(x,y,z,x+1,y+.5,z+1);
    }
    bool mayPick(int,bool)override{return true;}
};
class ClientSnow:public BoxRaycastTile {
    AABB* shape(BlockRaycaster* level,int x,int y,int z)override{
        return AABB::newTemp(0,0,0,1,2.0*(1+(level->getData(x,y,z)&7))/16.0,1);
    }
public:
    AABB* getAABB(BlockRaycaster* level,int x,int y,int z)override{
        return (level->getData(x,y,z)&7)>=3?AABB::newTemp(x,y,z,x+1,y+.5,z+1):nullptr;
    }
    bool mayPick(int,bool)override{return true;}
};
class ClientRayAccess:public RaycastAccess {
    const World& world;ClientCube cube{false},fluid{true};ClientStair stair;ClientSlab slab;ClientSnow snow;ClientDoor door;
public:
    explicit ClientRayAccess(const World& value):world(value){}
    int getTile(int x,int y,int z)override{return world.get(x,y,z);}
    int getData(int x,int y,int z)override{return world.getData(x,y,z);}
    RaycastTile* tileFor(int id)override{
        if(id==0)return nullptr;if(!validBlock(id))return nullptr;
        if(consoleIsStair(id))return &stair;
        if(id==44)return &slab;
        if(id==78)return &snow;
        if(id==64 || id==71 || id==65 || id==54 || id==85 || id==107 || id==113 || id==116 || id==117 || id==118)return &door;
        return id==8 || id==9 || id==10 || id==11?&fluid:&cube;
    }
};
}
Hit World::raycast(Vec3 origin,Vec3 direction,double reach)const{
    if(!std::isfinite(reach) || reach<0)return {};
    const double length=std::hypot(direction.x,direction.y,direction.z);
    if(!std::isfinite(length) || length<1e-12)return {};
    std::unique_ptr<::Vec3> a(::Vec3::newPermanent(origin.x,origin.y,origin.z));
    std::unique_ptr<::Vec3> b(::Vec3::newPermanent(origin.x+direction.x/length*reach,origin.y+direction.y/length*reach,origin.z+direction.z/length*reach));
    ClientRayAccess access(*this);BlockRaycaster ray(access);std::unique_ptr<HitResult> hit(ray.clip(a.get(),b.get()));
    if(!hit)return {};
    return {true,hit->x,hit->y,hit->z,hit->x+Facing::STEP_X[hit->f],hit->y+Facing::STEP_Y[hit->f],hit->z+Facing::STEP_Z[hit->f],
            std::hypot(hit->pos->x-origin.x,hit->pos->y-origin.y,hit->pos->z-origin.z)};
}
}
