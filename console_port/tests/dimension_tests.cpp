#include "DimensionChunkGenerator.h"
#include "ChunkStorage.h"
#include "net.minecraft.world.level.tile.h"
#include <iostream>
#include <future>
#include <climits>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("Expected rejection");}
int main(){try{
    using console::TerrainDimension;using console::DimensionChunkGenerator;using console::GenerationStage;
    for(auto dimension:{TerrainDimension::Nether,TerrainDimension::End}){
        DimensionChunkGenerator generator(dimension,8675309);auto origin=generator.generate(0,0);generator.generate(-8,7);generator.generate(8,-9);require(generator.generate(0,0).blocks==origin.blocks,"Dimension chunk depends on generation order");
        auto task=[&]{return generator.generate(0,0).blocks;};auto a=std::async(std::launch::async,task),b=std::async(std::launch::async,task);require(a.get()==origin.blocks && b.get()==origin.blocks,"Dimension generation races shared noise state");
        for(auto biome:origin.biomes)require(biome==(dimension==TerrainDimension::Nether?8:9),"Dimension biome assignment");
        console::ChunkStorage storage(origin);for(auto tile:storage.blocks)Tile::lightBlockFor(tile);
        rejects([&]{generator.generate(INT_MAX,0);});rejects([&]{generator.generate(0,INT_MIN);});rejects([&]{generator.generate(0,0,static_cast<GenerationStage>(99));});
    }
    DimensionChunkGenerator hell(TerrainDimension::Nether,12345678);
    bool carved=false,hasLava=false,hasWart=false;
    for(int x=-3;x<=3;++x)for(int z=-2;z<=2;++z){auto surface=hell.generate(x,z,GenerationStage::Surface),caves=hell.generate(x,z);carved|=surface.blocks!=caves.blocks;
        for(unsigned column=0;column<256;++column){require(surface.blocks[column*128]==7 && surface.blocks[column*128+127]==7,"Nether floor or ceiling bedrock missing");for(unsigned y=0;y<128;++y){auto before=surface.blocks[column*128+y],after=caves.blocks[column*128+y];hasLava|=after==11;hasWart|=after==115;require(before==after || ((before==87 || before==2 || before==3) && after==0),"Nether caves changed protected blocks");}}
    }
    require(carved && hasLava,"Nether caves and lava sea must occur");
    // The source's x/z surface indexing and random-thickness boundary walls are
    // intentionally retained; these faces are guaranteed solid at the rim.
    for(auto [cx,cz]:{std::pair{-9,0},std::pair{8,0},std::pair{0,-9},std::pair{0,8}}){auto edge=hell.generate(cx,cz);for(int across=0;across<16;++across)for(int y=0;y<128;++y){int x=cx==-9?0:cx==8?15:across;int z=cz==-9?0:cz==8?15:across;require(edge.blocks[(x*16+z)*128+y]==7,"Nether finite-world boundary face missing");}}
    DimensionChunkGenerator end(TerrainDimension::End,12345678);auto center=end.generate(0,0),far=end.generate(27,27);bool island=false;for(auto tile:center.blocks){require(tile==0 || tile==121,"End terrain contains an unexpected block");island|=tile==121;}require(island,"Central End island missing");for(auto tile:far.blocks)require(tile==0,"End terrain should fall off into distant void");require(end.generate(0,0,GenerationStage::Density).blocks==center.blocks,"End surface stage unexpectedly changed pure End stone");
    rejects([]{DimensionChunkGenerator invalid(TerrainDimension::Nether,0,54,0);});rejects([]{DimensionChunkGenerator invalid(TerrainDimension::Nether,0,53,3);});rejects([]{DimensionChunkGenerator invalid(static_cast<TerrainDimension>(0),0);});
    require(Tile::materialFor(115)==Material::plant && Tile::lightBlockFor(115)==0 && !Tile::solid[115],"Nether wart generation properties");
    std::cout<<"Dimension terrain, boundaries, carvers, biome fields and concurrency passed; wart in fixture="<<hasWart<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
