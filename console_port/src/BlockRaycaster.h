#pragma once
#include "AABB.h"
#include "HitResult.h"
namespace console {
class BlockRaycaster;
class RaycastTile {
public:
    virtual ~RaycastTile()=default;
    virtual AABB* getAABB(BlockRaycaster*,int x,int y,int z)=0;
    virtual bool mayPick(int data,bool liquid)=0;
    virtual HitResult* clip(BlockRaycaster*,int x,int y,int z,::Vec3* a,::Vec3* b)=0;
};
class RaycastAccess {
public:
    virtual ~RaycastAccess()=default;
    virtual int getTile(int x,int y,int z)=0;
    virtual int getData(int x,int y,int z)=0;
    virtual RaycastTile* tileFor(int id)=0;
};
// Original Level::clip traversal. It mutates a, as in the original. Inputs and
// tile shapes must outlive the call; a returned hit is owned but its pos is pooled.
class BlockRaycaster {
    RaycastAccess& access;
    RaycastTile* tileFor(int id){return access.tileFor(id);}
public:
    int getTile(int x,int y,int z){return access.getTile(x,y,z);}
    int getData(int x,int y,int z){return access.getData(x,y,z);}
    explicit BlockRaycaster(RaycastAccess& value):access(value){}
    HitResult* clip(::Vec3* a,::Vec3* b,bool liquid=false,bool solidOnly=false);
};
// Original base Tile::clip with an explicit, local-coordinate shape instead of
// mutable console TLS. Shape providers carry the real block/metadata behavior.
class BoxRaycastTile:public RaycastTile {
protected:
    virtual AABB* shape(BlockRaycaster*,int x,int y,int z)=0;
public:
    HitResult* clip(BlockRaycaster*,int x,int y,int z,::Vec3* a,::Vec3* b)override;
};
}
