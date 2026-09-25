#pragma once
// Stand-ins for what TileRenderer's shape methods touch; the methods
// themselves are ported/TileRender.cpp (tools/extract_tile_render.py). The
// world answers the queries a shape depends on, and the Tesselator collects
// the quads the methods emit (four vertices each, in world coordinates).
#include "Facing.h"
#include <functional>
#include <vector>

namespace console::tile_render {
using ::Facing;
struct SharedConstants {
    static constexpr bool TEXTURE_LIGHTING=true;
    static constexpr int WORLD_RESOLUTION=16;
};
// StitchedTexture over one 16x16 atlas slot: the adjusted UVs pull half a
// texel in from the icon's edges; getU/getV step in sixteenths.
struct Icon {
    float u0=0,v0=0,u1=0,v1=0;
    static constexpr float UVAdjust=(1.0f/16.0f)/256.0f;
    static Icon slot(int tile){return {(tile%16)/16.f,(tile/16)/16.f,(tile%16+1)/16.f,(tile/16+1)/16.f};}
    float getU0(bool adjust=false)const{return adjust?u0+UVAdjust:u0;}
    float getU1(bool adjust=false)const{return adjust?u1-UVAdjust:u1;}
    float getV0(bool adjust=false)const{return adjust?v0+UVAdjust:v0;}
    float getV1(bool adjust=false)const{return adjust?v1-UVAdjust:v1;}
    float getU(double offset,bool adjust=false)const{
        const float diff=getU1(adjust)-getU0(adjust);
        return getU0(adjust)+(diff*((float)offset/SharedConstants::WORLD_RESOLUTION));
    }
    float getV(double offset,bool adjust=false)const{
        const float diff=getV1(adjust)-getV0(adjust);
        return getV0(adjust)+(diff*((float)offset/SharedConstants::WORLD_RESOLUTION));
    }
};
struct TileVertex { float x,y,z,u,v; int light; };
class Tesselator {
public:
    std::vector<TileVertex>* out=nullptr;
    int light=0;
    static Tesselator* getInstance(){static thread_local Tesselator instance;return &instance;}
    void color(float,float,float){}
    void tex2(int packed){light=packed;}
    void vertexUV(float x,float y,float z,float u,float v){out->push_back({x,y,z,u,v,light});}
};
// The world the tile is in: its tiles and data, Level::isTopSolidBlocking,
// FireTile::canBurn (a tile with flame odds), each tile's packed light and
// its atlas slot for a face and data (Tile::getTexture).
struct LevelSource {
    std::function<int(int,int,int)> tile,data;
    std::function<bool(int,int,int)> topSolid,burnable;
    std::function<int(int,int,int)> light;
    std::function<int(int,int,int)> texture; // tile id, face, data -> atlas slot
    int getTile(int x,int y,int z){return tile(x,y,z);}
    int getData(int x,int y,int z){return data(x,y,z);}
    bool isTopSolidBlocking(int x,int y,int z){return topSolid(x,y,z);}
};
class FireTile;
class Tile {
public:
    int id=0;
    static inline int lightEmission[256]{};
    static inline FireTile* fire=nullptr;
    virtual ~Tile()=default;
    float getBrightness(LevelSource*,int,int,int){return 1;}
};
class FireTile:public Tile {
public:
    // FireTile::getTextureLayer: fire_0 and fire_1, both at atlas slot (15,1)
    // in PreStitchedTextureMap.
    Icon layers[2]{Icon::slot(31),Icon::slot(31)};
    Icon* getTextureLayer(int layer){return &layers[layer];}
    bool canBurn(LevelSource* level,int x,int y,int z){return level->burnable(x,y,z);}
};
class TileRenderer {
public:
    LevelSource* level=nullptr;
    Icon* fixedTexture=nullptr;
    Icon texture;
    bool hasFixedTexture(){return fixedTexture!=nullptr;}
    int getLightColor(Tile*,LevelSource* source,int x,int y,int z){return source->light(x,y,z);}
    Icon* getTexture(Tile* tile,int face,int data){texture=Icon::slot(level->texture(tile->id,face,data));return &texture;}
    bool tesselateFireInWorld(FireTile* tt,int x,int y,int z);
    bool tesselateTorchInWorld(Tile* tt,int x,int y,int z);
    void tesselateTorch(Tile* tt,float x,float y,float z,float xxa,float zza,int data);
};

// The quads of the tile at x, y, z (fire, torches), or none when the tile
// has no shape here.
inline std::vector<TileVertex> tesselate(LevelSource& level,int id,int x,int y,int z){
    static FireTile fire=[]{FireTile tile;tile.id=51;return tile;}();
    Tile::fire=&fire;
    std::vector<TileVertex> out;
    auto* t=Tesselator::getInstance();
    t->out=&out;
    TileRenderer renderer;renderer.level=&level;
    Tile tile;tile.id=id;
    switch(id){
    case 51:renderer.tesselateFireInWorld(&fire,x,y,z);break;
    case 50:case 75:case 76:renderer.tesselateTorchInWorld(&tile,x,y,z);break;
    default:break;
    }
    t->out=nullptr;
    return out;
}
}
