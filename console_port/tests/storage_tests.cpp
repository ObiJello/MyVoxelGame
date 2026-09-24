#include "GenerationRegion.h"
#include "WorldGenLevel.h"
#include "net.minecraft.world.level.tile.h"
#include <future>
#include <iostream>
#include <stdexcept>
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){
    try{
        std::vector<std::future<Material*>> materials;
        for(int i=0;i<8;++i)materials.push_back(std::async(std::launch::async,[]{Material::staticCtor();return Material::water;}));
        for(auto& material:materials)require(material.get()==Material::water,"Material initialization raced");
        auto* originalWater=Material::water;Material::staticCtor();
        require(Material::water==originalWater,"Repeated material initialization replaced registry pointers");
        require(MaterialColor::colors[14]==nullptr && MaterialColor::colors[15]==nullptr,"Unused color registry entries must be empty");
        require(Material::water->isLiquid() && !Material::water->blocksMotion(),"Water material semantics differ");
        require(Material::glass->isSolid() && !Material::glass->isSolidBlocking(),"Glass material semantics differ");
        require(Tile::lightBlock[20]==0 && Tile::lightBlock[18]==1 && Tile::lightBlock[9]==3 && Tile::lightBlock[44]==255,"Generation opacity overrides differ");
        require(Tile::materialFor(127)==Material::plant && Tile::materialFor(97)==Material::clay && Tile::materialFor(88)==Material::sand,"Special tile material mappings differ");
        console::ChunkStorage chunk;
        bool unsupported=false;try{chunk.set(0,64,0,200);}catch(const std::out_of_range&){unsupported=true;}
        require(unsupported && chunk.blocks[64]==0,"Unknown tile properties were silently accepted or modified storage");
        for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<256;++y)chunk.metadata.set(x,y,z,(x*3+z*5+y)&15);
        for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int pair=0;pair<128;++pair){
            int low=(x*3+z*5+pair*2)&15,high=(low+1)&15;
            require(chunk.metadata.data[(x*16+z)*128+pair]==(low|(high<<4)),"Packed metadata order differs");
            require(chunk.metadata.get(x,pair*2,z)==low && chunk.metadata.get(x,pair*2+1,z)==high,"Nibble roundtrip differs");
        }
        for(int value=0;value<16;++value){chunk.skyLight.setAll(value);for(unsigned i=0;i<chunk.skyLight.data.length;++i)
            require(chunk.skyLight.data[i]==(value|(value<<4)),"DataLayer setAll did not fill both nibbles");}
        require(chunk.set(1,255,3,17,2),"Highest build-height block rejected");
        require(chunk.heightmap[19]==256,"Heightmap did not reach build limit");
        require(!chunk.set(1,256,3,17,2) && !chunk.set(1,2,3,17,16),"Invalid storage input accepted");
        chunk.set(1,127,3,1);chunk.set(1,255,3,0);require(chunk.heightmap[19]==128,"Heightmap did not lower after removal");
        chunk.set(1,200,3,20);chunk.set(1,210,3,106);chunk.set(1,215,3,127);
        require(chunk.heightmap[19]==128,"Transparent vegetation or glass raised the generation heightmap");
        chunk.set(1,180,3,18);require(chunk.heightmap[19]==181,"Leaves did not raise heightmap");
        chunk.set(1,190,3,9);require(chunk.heightmap[19]==191,"Water did not raise heightmap");
        chunk.set(1,190,3,20);require(chunk.heightmap[19]==181,"Replacing water with glass did not lower heightmap");
        chunk.recalculateHeightmap();require(chunk.heightmap[19]==181,"Full heightmap scan differs from incremental updates");
        chunk.set(1,180,3,0);require(chunk.heightmap[19]==128,"Transparent blocks prevented heightmap descent");
        console::GenerationRegion region;region.insert(-1,-1,std::make_unique<console::ChunkStorage>());
        require(region.setTileAndData(-1,250,-16,17,3),"Negative world coordinate placement failed");
        require(region.getTile(-1,250,-16)==17 && region.getData(-1,250,-16)==3,"Negative coordinate metadata mapping failed");
        require(region.getHeightmap(-1,-16)==251,"Negative coordinate heightmap failed");
        bool missing=false;try{region.getTile(0,1,0);}catch(const std::out_of_range&){missing=true;}
        require(missing,"Missing decoration neighborhood was silently treated as air");
        console::GenerationRegion high;auto top=std::make_unique<console::ChunkStorage>();top->set(1,255,3,17);
        high.insert(0,0,std::move(top));Level level(1);level.setBlockAccess(high);high.initializeLight(level);
        require(high.getHeightmap(1,3)==256,"Original chunk-light adapter truncated the top build-height column");
        require(level.getBrightness(LightLayer::Sky,1,255,3)==0,"Highest opaque block leaked skylight");
        bool halo=false;try{level.getDaytimeRawBrightness(1,254,3);}catch(const std::logic_error&){halo=true;}
        require(halo,"Incomplete generated lighting halo was accepted");
        for(int x=0;x<16;++x)for(int z=0;z<16;++z)high.chunks.at({0,0})->set(x,255,z,1);
        high.initializeLight(level);
        require(high.chunks.at({0,0})->minHeight==256,"Full-ceiling chunk minimum height was truncated");
        std::cout<<"Passed nibble layout/fill, block metadata, build height, heightmap updates and negative-coordinate region checks\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
