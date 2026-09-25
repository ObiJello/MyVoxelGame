#pragma once
// Stand-ins for what TileRenderer::tesselateFireInWorld touches; the method
// itself is ported/FireRender.cpp (tools/extract_fire_render.py). The world
// answers the two queries the shape depends on, and the Tesselator collects
// the quads it emits (four vertices each, in world coordinates).
#include <array>
#include <functional>
#include <vector>

namespace console::fire_render {
struct SharedConstants { static constexpr bool TEXTURE_LIGHTING=true; };
// StitchedTexture: the adjusted UVs pull half a texel in from the icon's edges.
struct Icon {
    float u0,v0,u1,v1;
    static constexpr float UVAdjust=(1.0f/16.0f)/256.0f;
    float getU0(bool adjust=false)const{return adjust?u0+UVAdjust:u0;}
    float getU1(bool adjust=false)const{return adjust?u1-UVAdjust:u1;}
    float getV0(bool adjust=false)const{return adjust?v0+UVAdjust:v0;}
    float getV1(bool adjust=false)const{return adjust?v1-UVAdjust:v1;}
};
struct FireVertex { float x,y,z,u,v; int light; };
class Tesselator {
public:
    std::vector<FireVertex>* out=nullptr;
    int light=0;
    static Tesselator* getInstance(){static thread_local Tesselator instance;return &instance;}
    void color(float,float,float){}
    void tex2(int packed){light=packed;}
    void vertexUV(float x,float y,float z,float u,float v){out->push_back({x,y,z,u,v,light});}
};
// The world the fire is in: Level::isTopSolidBlocking, FireTile::canBurn
// (a tile with flame odds) and the tile's packed light.
struct LevelSource {
    std::function<bool(int,int,int)> topSolid,burnable;
    std::function<int(int,int,int)> light;
    bool isTopSolidBlocking(int x,int y,int z){return topSolid(x,y,z);}
};
class FireTile {
public:
    // FireTile::getTextureLayer: fire_0 and fire_1, both at atlas slot (15,1)
    // in PreStitchedTextureMap.
    Icon layers[2]{{15/16.f,1/16.f,16/16.f,2/16.f},{15/16.f,1/16.f,16/16.f,2/16.f}};
    Icon* getTextureLayer(int layer){return &layers[layer];}
    float getBrightness(LevelSource*,int,int,int){return 1;}
    bool canBurn(LevelSource* level,int x,int y,int z){return level->burnable(x,y,z);}
};
struct Tile { static inline FireTile* fire=nullptr; };
class TileRenderer {
public:
    LevelSource* level=nullptr;
    Icon* fixedTexture=nullptr;
    bool hasFixedTexture(){return fixedTexture!=nullptr;}
    int getLightColor(FireTile*,LevelSource* source,int x,int y,int z){return source->light(x,y,z);}
    bool tesselateFireInWorld(FireTile* tt,int x,int y,int z);
};
// The quads of the fire tile at x, y, z.
inline std::vector<FireVertex> tesselateFire(LevelSource& level,int x,int y,int z){
    static FireTile fire;
    Tile::fire=&fire;
    std::vector<FireVertex> out;
    auto* t=Tesselator::getInstance();
    t->out=&out;
    TileRenderer renderer;renderer.level=&level;
    renderer.tesselateFireInWorld(&fire,x,y,z);
    t->out=nullptr;
    return out;
}
}
