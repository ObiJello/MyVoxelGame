#pragma once
// Stand-ins for what TileRenderer's shape methods touch; the methods
// themselves are ported/TileRender.cpp (tools/extract_tile_render.py). The
// world answers the queries a shape depends on, and the Tesselator collects
// the quads the methods emit (four vertices each, in world coordinates).
#include "Direction.h"
#include "Facing.h"
#include "Vec3.h"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace console::tile_render {
using ::Direction;
using ::Facing;
using ::Vec3;
using std::wstring;
using byte=unsigned char;
// Definitions.h
constexpr float PI=3.141592654f;
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
struct TileVertex { float x,y,z,u,v,r,g,b; int light; };
class Tesselator {
public:
    std::vector<TileVertex>* out=nullptr;
    int light=0;
    float r=1,g=1,b=1;
    static Tesselator* getInstance(){static thread_local Tesselator instance;return &instance;}
    void color(float red,float green,float blue){r=red;g=green;b=blue;}
    void tex2(int packed){light=packed;}
    void vertexUV(float x,float y,float z,float u,float v){out->push_back({x,y,z,u,v,r,g,b,light});}
};
// The world the tile is in: its tiles and data, Level::isTopSolidBlocking and
// isSolidBlockingTile, FireTile::canBurn (a tile with flame odds),
// RedStoneDustTile::shouldConnectTo, each tile's packed light, its atlas slot
// for a face (Facing) and data (Tile::getTexture) and its current shape
// (Tile::updateShape).
struct LevelSource {
    std::function<int(int,int,int)> tile,data;
    std::function<bool(int,int,int)> topSolid,solidBlocking,burnable;
    std::function<bool(int,int,int,int)> dustConnects;
    std::function<int(int,int,int)> light;
    std::function<int(int,int,int)> texture; // tile id, face, data -> atlas slot
    std::function<std::array<float,6>(int,int,int)> shape;
    int getTile(int x,int y,int z){return tile(x,y,z);}
    int getData(int x,int y,int z){return data(x,y,z);}
    bool isTopSolidBlocking(int x,int y,int z){return topSolid(x,y,z);}
    bool isSolidBlockingTile(int x,int y,int z){return solidBlocking(x,y,z);}
};
class FireTile;
class Tile {
public:
    static const int redStoneDust_Id=55,stoneBrick_Id=4;
    int id=0;
    static inline int lightEmission[256]{};
    static inline FireTile* fire=nullptr;
    static inline Tile* stoneBrick=nullptr;
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
class DiodeTile:public Tile {
public:
    static const int DIRECTION_MASK=3,DELAY_MASK=0xC,DELAY_SHIFT=2;
    // DiodeTile.cpp
    static constexpr double DELAY_RENDER_OFFSETS[4]={-1.0f/16.0f,1.0f/16.0f,3.0f/16.0f,5.0f/16.0f};
};
// PistonBaseTile's facing and icons (piston_side, piston_top,
// piston_top_sticky, piston_inner_top in PreStitchedTextureMap).
class PistonBaseTile:public Tile {
public:
    static const int EXTENDED_BIT=8;
    static constexpr float PLATFORM_THICKNESS=4.0f;
    static inline const wstring EDGE_TEX=L"piston_side",PLATFORM_TEX=L"piston_top",
        PLATFORM_STICKY_TEX=L"piston_top_sticky",INSIDE_TEX=L"piston_inner_top";
    static int getFacing(int data){return data&0x7;}
    static Icon* getTexture(const wstring& name){
        static Icon edge=Icon::slot(108),platform=Icon::slot(107),sticky=Icon::slot(106),inside=Icon::slot(110);
        return name==EDGE_TEX?&edge:name==PLATFORM_TEX?&platform:name==PLATFORM_STICKY_TEX?&sticky:&inside;
    }
    // The source's way of telling getTexture to use the inside icon for an
    // extended piston's front; the texture callback reads the extended bit.
    void updateShape(float,float,float,float,float,float){}
};
class PistonExtensionTile:public Tile {
public:
    static int getFacing(int data){return data&0x7;}
};
// RedStoneDustTile's four icons (PreStitchedTextureMap slots) and
// shouldConnectTo, answered by the world.
class RedStoneDustTile:public Tile {
public:
    static inline const wstring TEXTURE_CROSS=L"redstoneDust_cross",TEXTURE_LINE=L"redstoneDust_line",
        TEXTURE_CROSS_OVERLAY=L"redstoneDust_cross_overlay",TEXTURE_LINE_OVERLAY=L"redstoneDust_line_overlay";
    static Icon* getTexture(const wstring& name){
        static Icon cross=Icon::slot(164),line=Icon::slot(165),crossOverlay=Icon::slot(180),lineOverlay=Icon::slot(181);
        return name==TEXTURE_CROSS?&cross:name==TEXTURE_LINE?&line:name==TEXTURE_CROSS_OVERLAY?&crossOverlay:&lineOverlay;
    }
    static bool shouldConnectTo(LevelSource* level,int x,int y,int z,int direction){return level->dustConnects(x,y,z,direction);}
};
// Minecraft::getColourTable: the redstone dust entries of colours.xml.
enum eMinecraftColour {
    eMinecraftColour_Tile_RedstoneDustUnlit,eMinecraftColour_Tile_RedstoneDustLitMin,eMinecraftColour_Tile_RedstoneDustLitMax
};
struct ColourTable {
    unsigned int getColor(eMinecraftColour colour)const{
        switch(colour){
        case eMinecraftColour_Tile_RedstoneDustUnlit:return 0x4c0000;
        case eMinecraftColour_Tile_RedstoneDustLitMin:return 0x700000;
        case eMinecraftColour_Tile_RedstoneDustLitMax:return 0xff3200;
        }
        return 0;
    }
};
struct Minecraft {
    ColourTable colours;
    static Minecraft* GetInstance(){static Minecraft instance;return &instance;}
    ColourTable* getColourTable(){return &colours;}
};
class TileRenderer {
public:
    LevelSource* level=nullptr;
    Icon* fixedTexture=nullptr;
    Icon texture,fixedIcon;
    // TileRenderer::setShape: the box tesselateBlockInWorld draws, and the
    // faces it draws (Tile::shouldRenderFace; the repeater leaves out its top
    // and bottom).
    float tileShapeX0=0,tileShapeY0=0,tileShapeZ0=0,tileShapeX1=1,tileShapeY1=1,tileShapeZ1=1;
    int faceMask=0x3f;
    // Per-face texture rotation (TileRenderer's northFlip...upFlip).
    static const int FLIP_NONE=0,FLIP_CW=1,FLIP_CCW=2,FLIP_180=3;
    int northFlip=FLIP_NONE,southFlip=FLIP_NONE,eastFlip=FLIP_NONE,westFlip=FLIP_NONE,upFlip=FLIP_NONE,downFlip=FLIP_NONE;
    bool noCulling=false;
    void setShape(float x0,float y0,float z0,float x1,float y1,float z1){
        tileShapeX0=x0;tileShapeY0=y0;tileShapeZ0=z0;tileShapeX1=x1;tileShapeY1=y1;tileShapeZ1=z1;
    }
    bool hasFixedTexture(){return fixedTexture!=nullptr;}
    void setFixedTexture(Icon* icon){fixedIcon=*icon;fixedTexture=&fixedIcon;}
    void clearFixedTexture(){fixedTexture=nullptr;}
    int getLightColor(Tile*,LevelSource* source,int x,int y,int z){return source->light(x,y,z);}
    Icon* getTexture(Tile* tile,int face,int data){texture=Icon::slot(level->texture(tile->id,face,data));return &texture;}
    Icon* getTexture(Tile* tile,int face){return getTexture(tile,face,0);}
    Icon* getTexture(Tile* tile){return getTexture(tile,Facing::UP,0);}
    // tesselateBlockInWorld without ambient occlusion: the current shape's
    // faces with TileRenderer's face shading (0.5 down, 0.8 north/south,
    // 0.6 west/east) and the face's texture over the shape's extent.
    bool tesselateBlockInWorld(Tile* tt,int x,int y,int z);
    bool tesselateFireInWorld(FireTile* tt,int x,int y,int z);
    bool tesselateTorchInWorld(Tile* tt,int x,int y,int z);
    void tesselateTorch(Tile* tt,float x,float y,float z,float xxa,float zza,int data);
    bool tesselateDiodeInWorld(DiodeTile* tt,int x,int y,int z);
    void tesselateDiodeInWorld(DiodeTile* tt,int x,int y,int z,int dir);
    bool tesselateLeverInWorld(Tile* tt,int x,int y,int z);
    bool tesselateDustInWorld(Tile* tt,int x,int y,int z);
    bool tesselatePistonBaseInWorld(Tile* tt,int x,int y,int z,bool forceExtended,int forceData=-1);
    void renderPistonArmUpDown(float x0,float x1,float y0,float y1,float z0,float z1,float br,float armLengthPixels);
    void renderPistonArmNorthSouth(float x0,float x1,float y0,float y1,float z0,float z1,float br,float armLengthPixels);
    void renderPistonArmEastWest(float x0,float x1,float y0,float y1,float z0,float z1,float br,float armLengthPixels);
    bool tesselatePistonExtensionInWorld(Tile* tt,int x,int y,int z,bool fullArm,int forceData=-1);
};

inline bool TileRenderer::tesselateBlockInWorld(Tile* tt,int x,int y,int z){
    auto* t=Tesselator::getInstance();
    t->tex2(getLightColor(tt,level,x,y,z));
    const int data=level->getData(x,y,z);
    const float X0=x+tileShapeX0,X1=x+tileShapeX1,Y0=y+tileShapeY0,Y1=y+tileShapeY1,Z0=z+tileShapeZ0,Z1=z+tileShapeZ1;
    const int flips[6]{downFlip,upFlip,northFlip,southFlip,westFlip,eastFlip};
    auto face=[&](int facing,float shade,const std::array<std::array<float,3>,4>& p,float ua,float ub,float va,float vb){
        if(!(faceMask&(1<<facing)))return;
        Icon* tex=hasFixedTexture()?fixedTexture:getTexture(tt,facing,data);
        t->color(shade,shade,shade);
        const float us[4]{ua,ua,ub,ub},vs[4]{va,vb,vb,va};
        // Rotate the texture a quarter turn per step (clockwise takes each
        // corner's coordinates from the next corner round).
        const int turn=flips[facing]==FLIP_CW?1:flips[facing]==FLIP_CCW?3:flips[facing]==FLIP_180?2:0;
        for(int i=0;i<4;++i){
            const int k=(i+turn)%4;
            t->vertexUV(p[i][0],p[i][1],p[i][2],tex->getU(us[k]*16),tex->getV(vs[k]*16));
        }
    };
    // Outward winding, first corner at the face's top left.
    face(Facing::DOWN,.5f,{{{X0,Y0,Z1},{X0,Y0,Z0},{X1,Y0,Z0},{X1,Y0,Z1}}},tileShapeX0,tileShapeX1,tileShapeZ1,tileShapeZ0);
    face(Facing::UP,1.f,{{{X0,Y1,Z0},{X0,Y1,Z1},{X1,Y1,Z1},{X1,Y1,Z0}}},tileShapeX0,tileShapeX1,tileShapeZ0,tileShapeZ1);
    face(Facing::NORTH,.8f,{{{X1,Y1,Z0},{X1,Y0,Z0},{X0,Y0,Z0},{X0,Y1,Z0}}},1-tileShapeX1,1-tileShapeX0,1-tileShapeY1,1-tileShapeY0);
    face(Facing::SOUTH,.8f,{{{X0,Y1,Z1},{X0,Y0,Z1},{X1,Y0,Z1},{X1,Y1,Z1}}},tileShapeX0,tileShapeX1,1-tileShapeY1,1-tileShapeY0);
    face(Facing::WEST,.6f,{{{X0,Y1,Z0},{X0,Y0,Z0},{X0,Y0,Z1},{X0,Y1,Z1}}},tileShapeZ0,tileShapeZ1,1-tileShapeY1,1-tileShapeY0);
    face(Facing::EAST,.6f,{{{X1,Y1,Z1},{X1,Y0,Z1},{X1,Y0,Z0},{X1,Y1,Z0}}},1-tileShapeZ1,1-tileShapeZ0,1-tileShapeY1,1-tileShapeY0);
    t->color(1,1,1);
    return true;
}

// The quads of the tile at x, y, z, or none when the tile has no shape here:
// fire, torches, redstone dust, levers, repeaters, pistons and their heads,
// and the plain shaped blocks (buttons, pressure plates, trapdoors) drawn as
// their updateShape box.
inline std::vector<TileVertex> tesselate(LevelSource& level,int id,int x,int y,int z){
    static FireTile fire=[]{FireTile tile;tile.id=51;return tile;}();
    static Tile cobble=[]{Tile tile;tile.id=Tile::stoneBrick_Id;return tile;}();
    Tile::fire=&fire;Tile::stoneBrick=&cobble;
    std::vector<TileVertex> out;
    auto* t=Tesselator::getInstance();
    t->out=&out;t->color(1,1,1);
    TileRenderer renderer;renderer.level=&level;
    DiodeTile tile;tile.id=id;
    const auto box=[&]{const auto b=level.shape(x,y,z);renderer.setShape(b[0],b[1],b[2],b[3],b[4],b[5]);};
    switch(id){
    case 51:renderer.tesselateFireInWorld(&fire,x,y,z);break;
    case 50:case 75:case 76:renderer.tesselateTorchInWorld(&tile,x,y,z);break;
    case 55:renderer.tesselateDustInWorld(&tile,x,y,z);break;
    case 69:renderer.tesselateLeverInWorld(&tile,x,y,z);break;
    case 93:case 94:
        // DiodeTile::shouldRenderFace: up and down are the shape renderer's.
        box();renderer.faceMask=0x3c;renderer.tesselateDiodeInWorld(&tile,x,y,z);break;
    case 70:case 72:case 77:case 96:case 143:box();renderer.tesselateBlockInWorld(&tile,x,y,z);break;
    case 29:case 33:{PistonBaseTile base;base.id=id;renderer.tesselatePistonBaseInWorld(&base,x,y,z,false);break;}
    case 34:{Tile head;head.id=id;renderer.tesselatePistonExtensionInWorld(&head,x,y,z,true);break;}
    default:break;
    }
    t->out=nullptr;
    return out;
}

// PistonPieceRenderer: a moving piece is drawn where the piece entity has got
// to. A retracting piston's own base draws extended with its arm; a head
// short of halfway draws with a half arm; anything else is its own shape
// (a full cube for ordinary blocks).
inline std::vector<TileVertex> tesselateMovingPiece(LevelSource& level,int id,int x,int y,int z,
                                                    bool sourcePiston,bool extending,float progress){
    std::vector<TileVertex> out;
    auto* t=Tesselator::getInstance();
    t->out=&out;t->color(1,1,1);
    TileRenderer renderer;renderer.level=&level;renderer.noCulling=true;
    if(id==34 && progress<.5f){Tile head;head.id=id;renderer.tesselatePistonExtensionInWorld(&head,x,y,z,false);}
    else if(sourcePiston && !extending){PistonBaseTile base;base.id=id;renderer.tesselatePistonBaseInWorld(&base,x,y,z,true);}
    else{
        t->out=nullptr;
        out=tesselate(level,id,x,y,z);
        if(out.empty()){
            t->out=&out;
            Tile tile;tile.id=id;renderer.setShape(0,0,0,1,1,1);renderer.tesselateBlockInWorld(&tile,x,y,z);
        }
    }
    t->out=nullptr;
    return out;
}
}
