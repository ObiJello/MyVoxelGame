#pragma once
#include <array>
#include "Material.h"
#include <mutex>
#include <stdexcept>
#include "Direction.h"
#include "Facing.h"
class Level;
class Random;
struct Tile {
    int id;
    Material* material;
    explicit Tile(int id):id(id),material(materialFor(id)){}
    virtual ~Tile()=default;
    virtual void tick(Level*,int,int,int,Random*){}
    virtual bool mayPlace(Level*,int,int,int);
    virtual bool mayPlace(Level*,int,int,int,int);
    virtual bool canSurvive(Level*,int,int,int){throw std::logic_error("Tile survival rule has not been ported");}
    virtual bool isCubeShaped()const{return solid[id] || id==18 || id==20 || id==79;}
    static Tile* tiles[256];
    static Tile *reeds,*cactus,*waterLily,*pumpkin,*vine;
    static constexpr int rock_Id=1,grass_Id=2,dirt_Id=3,unbreakable_Id=7,
        water_Id=8,calmWater_Id=9,lava_Id=10,sand_Id=12,treeTrunk_Id=17,leaves_Id=18,
        sandStone_Id=24,ice_Id=79,vine_Id=106,cocoa_Id=127,sapling_Id=6,
        clay_Id=82,mycel_Id=110,hugeMushroom1_Id=99,stoneSlabHalf_Id=44,
        farmland_Id=60,cactus_Id=81,reeds_Id=83,waterLily_Id=111,pumpkin_Id=86,
        calmLava_Id=11,gravel_Id=13,hellRock_Id=87,hellSand_Id=88,netherStalk_Id=115,whiteStone_Id=121;
    // Opaque full-cube entries used by the current generation region. This is a
    // limited generation registry; the complete original Tile registry is pending.
    static inline const std::array<bool,256> solid=[] {
        std::array<bool,256> flags{};
        for(int id:{1,2,3,4,5,7,12,13,14,15,16,17,21,22,24,35,41,42,43,45,47,48,49,56,57,73,74,80,82,86,87,88,89,97,98,99,100,110,112,121,129})flags[id]=true;
#define CONSOLE_TUTORIAL_TILE(id,material,opaque,opacity,emission) flags[id]=opaque;
#include "TutorialTileProperties.inc"
#undef CONSOLE_TUTORIAL_TILE
        return flags;
    }();
    // Tile::staticCtor overrides plus HalfSlabTile's light-block assignment.
    // Transparent blocks still have solid materials; these are distinct queries.
    static inline const std::array<unsigned char,256> lightBlock=[] {
        std::array<unsigned char,256> values{};
        for(int id=0;id<256;++id)values[id]=solid[id]?255:0;
        for(int id:{8,9,79})values[id]=3;
        for(int id:{18,30})values[id]=1;
        for(int id:{10,11,44,60})values[id]=255;
#define CONSOLE_TUTORIAL_TILE(id,material,opaque,opacity,emission) values[id]=opacity;
#include "TutorialTileProperties.inc"
#undef CONSOLE_TUTORIAL_TILE
        return values;
    }();
    static inline const std::array<bool,256> supported=[] {
        auto values=solid;
#define CONSOLE_TUTORIAL_TILE(id,material,opaque,opacity,emission) values[id]=true;
#include "TutorialTileProperties.inc"
#undef CONSOLE_TUTORIAL_TILE
        for(int id:{0,6,8,9,10,11,18,20,30,31,32,37,38,39,40,44,60,79,81,83,106,111,115,127})values[id]=true;
        return values;
    }();
    static inline const std::array<unsigned char,256> lightEmission=[] {
        std::array<unsigned char,256> values{};
        values[10]=values[11]=values[89]=15;
        values[39]=1;values[74]=9;
#define CONSOLE_TUTORIAL_TILE(id,material,opaque,opacity,emission) values[id]=emission;
#include "TutorialTileProperties.inc"
#undef CONSOLE_TUTORIAL_TILE
        return values;
    }();
    static int lightBlockFor(int id) {
        if(id<0 || id>=256 || !supported[id])
            throw std::out_of_range("Tile light properties have not been ported for block "+std::to_string(id));
        return lightBlock[id];
    }
    static Material* materialFor(int id) {
        // Also guards the untouched reference registry, whose staticCtor assumes
        // one startup call. Registry objects have the original process lifetime.
        static std::once_flag initialized;
        std::call_once(initialized,[]{MaterialColor::staticCtor();Material::staticCtor();});
        switch(id) {
#define CONSOLE_TUTORIAL_TILE(id,material,opaque,opacity,emission) case id:return Material::material;
#include "TutorialTileProperties.inc"
#undef CONSOLE_TUTORIAL_TILE
        case 0:return Material::air;
        case 2:case 110:return Material::grass;
        case 3:case 60:return Material::dirt;
        case 5:case 17:case 47:case 99:case 100:return Material::wood;
        case 6:case 37:case 38:case 39:case 40:case 83:case 111:case 115:case 127:return Material::plant;
        case 8:case 9:return Material::water;
        case 10:case 11:return Material::lava;
        case 12:case 13:case 88:return Material::sand;
        case 18:return Material::leaves;
        case 20:return Material::glass;
        case 30:return Material::web;
        case 35:return Material::cloth;
        case 41:case 42:case 57:return Material::metal;
        case 79:return Material::ice;
        case 80:return Material::snow;
        case 81:return Material::cactus;
        case 86:return Material::vegetable;
        case 82:case 97:return Material::clay;
        case 89:return Material::glass;
        case 31:case 32:case 106:return Material::replaceable_plant;
        case 1:case 4:case 7:case 14:case 15:case 16:case 21:case 22:case 24:
        case 43:case 44:case 45:case 48:case 49:case 56:case 73:case 74:
        case 87:case 98:case 112:case 121:case 129:return Material::stone;
        default:throw std::out_of_range("Tile material has not been ported to the generation registry");
        }
    }
};
struct StoneSlabTile {static constexpr int SAND_SLAB=1;};
struct VineTile:Tile {
    using Tile::Tile;
    static constexpr int VINE_SOUTH=1,VINE_WEST=2,VINE_NORTH=4,VINE_EAST=8;
    bool mayPlace(Level*,int,int,int,int)override;
    bool isAcceptableNeighbor(int);
};
struct LeafTile {static constexpr int EVERGREEN_LEAF=1,BIRCH_LEAF=2;};
struct TreeTile {static constexpr int DARK_TRUNK=1,BIRCH_TRUNK=2,FACING_Y=0,FACING_X=4,FACING_Z=8;};

// Generation-only class surfaces for the extracted original placement methods.
// They do not implement simulation, drops, drawing, or ticking.
struct Bush:Tile {
    using Tile::Tile;
    bool mayPlace(Level*,int,int,int)override;
    virtual bool mayPlaceOn(int);
    bool canSurvive(Level*,int,int,int)override;
};
struct DeadBushTile:Bush {using Bush::Bush;bool mayPlaceOn(int)override;};
struct Mushroom:Bush {
    using Bush::Bush;
    bool mayPlace(Level*,int,int,int)override;
    bool mayPlaceOn(int)override;
    bool canSurvive(Level*,int,int,int)override;
};
struct CactusTile:Tile {using Tile::Tile;bool mayPlace(Level*,int,int,int)override;bool canSurvive(Level*,int,int,int)override;};
struct ReedTile:Tile {using Tile::Tile;bool mayPlace(Level*,int,int,int)override;bool canSurvive(Level*,int,int,int)override;};
struct WaterlilyTile:Bush {using Bush::Bush;bool mayPlaceOn(int)override;bool canSurvive(Level*,int,int,int)override;};
struct PumpkinTile:Tile {using Tile::Tile;bool mayPlace(Level*,int,int,int)override;};
struct HalfSlabTile:Tile {using Tile::Tile;static constexpr int TOP_SLOT_BIT=8;};
struct StairTile:Tile {using Tile::Tile;static constexpr int UPSIDEDOWN_BIT=4;};
struct TopSnowTile:Tile {using Tile::Tile;static constexpr int HEIGHT_MASK=7,MAX_HEIGHT=6;};
void initializeGenerationTiles();
