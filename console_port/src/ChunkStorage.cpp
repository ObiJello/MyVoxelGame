#include "ChunkStorage.h"
#include "net.minecraft.world.level.tile.h"
#include <algorithm>
#include <stdexcept>
namespace console {
ChunkStorage::ChunkStorage(const GeneratedChunk& generated):biomes(generated.biomes){
    for(int i=0;i<256;++i)std::copy_n(generated.blocks.data()+i*128,128,blocks.data()+i*height);
    recalculateHeightmap();
}
bool ChunkStorage::set(int x,int y,int z,int tile,int data){
    if(x<0 || x>=16 || z<0 || z>=16 || y<0 || y>=height || tile<0 || tile>255 || data<0 || data>15)return false;
    const int lightBlock=Tile::lightBlockFor(tile);
    int column=x*16+z,offset=column*height+y;
    if(blocks[offset]==tile && metadata.get(x,y,z)==data)return false;
    blocks[offset]=static_cast<std::uint8_t>(tile);metadata.set(x,y,z,data);
    if(Tile::lightEmission[tile]>0)emissiveAdded=true;
    unsaved=true;
    if(lightBlock!=0 && y>=heightmap[column])heightmap[column]=y+1;
    else if(lightBlock==0 && y+1==heightmap[column]){
        int top=y;while(top>0 && Tile::lightBlock[blocks[column*height+top-1]]==0)--top;
        heightmap[column]=top;
    }
    return true;
}
void ChunkStorage::recalculateHeightmap(){
    for(int i=0;i<256;++i){int y=height;while(y>0 && Tile::lightBlockFor(blocks[i*height+y-1])==0)--y;heightmap[i]=y;}
}
}
