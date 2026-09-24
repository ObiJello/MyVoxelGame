#include "ChunkRecord.h"
#include <iostream>
#include <vector>
static void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
static auto save(console::ChunkRecord& record){ByteArrayOutputStream bytes;DataOutputStream output(&bytes);record.write(&output);return std::vector<unsigned char>(bytes.buf.data,bytes.buf.data+bytes.size());}
static auto load(const std::vector<unsigned char>& bytes){ByteArrayInputStream input(byteArray(const_cast<unsigned char*>(bytes.data()),bytes.size()));struct Detach{ByteArrayInputStream& input;~Detach(){input.reset();}} detach{input};DataInputStream reader(&input);auto result=console::ChunkRecord::read(&reader);require(input.read()==-1,"Chunk reader must consume exactly one record");return result;}
int main(){try{
    console::ChunkRecord record;record.x=INT_MIN;record.z=INT_MAX;record.lastUpdate=INT64_MAX;record.terrainPopulated=console::ChunkRecord::allNeighbours;
    record.lowerBlocks->set(1,127,2,254);record.upperBlocks->set(3,0,4,5);record.lowerData->set(1,127,2,7);record.upperData->set(3,0,4,12);
    record.lowerSkyLight->set(1,127,2,6);record.upperSkyLight->set(3,0,4,9);record.lowerBlockLight->set(1,127,2,4);record.upperBlockLight->set(3,0,4,11);
    for(int i=0;i<256;++i){record.heightValues[i]=i;record.biomeValues[i]=255-i;}
    auto* entities=new ListTag<CompoundTag>;auto* entity=new CompoundTag;entity->putString(L"id",L"Pig");entity->putInt(L"Age",-123);entities->add(entity);record.extra->put(L"Entities",entities);
    unsigned char unknown[]={9,8,7,6};record.extra->putByteArray(L"UnknownFuturePayload",byteArray(unknown,4));
    auto bytes=save(record);require(bytes[0]==0 && bytes[1]==SAVE_FILE_VERSION_COMPRESSED_CHUNK_STORAGE,"Original version prefix");
    auto decoded=load(bytes);require(decoded->x==INT_MIN && decoded->z==INT_MAX && decoded->lastUpdate==INT64_MAX,"Chunk coordinates and full 64-bit update time");
    require(decoded->terrainPopulated==(console::ChunkRecord::allNeighbours|console::ChunkRecord::postProcessed),"Original neighbor-completion migration");
    require(decoded->heightValues==record.heightValues && decoded->biomeValues==record.biomeValues,"Raw byte heightmap and biome layout retained");
    require(decoded->lowerBlocks->get(1,127,2)==254 && decoded->upperBlocks->get(3,0,4)==5,"Both tile sections retained");
    require(decoded->lowerData->get(1,127,2)==7 && decoded->upperData->get(3,0,4)==12,"Both metadata sections retained");
    require(decoded->lowerSkyLight->get(1,127,2)==6 && decoded->upperSkyLight->get(3,0,4)==9 && decoded->lowerBlockLight->get(1,127,2)==4 && decoded->upperBlockLight->get(3,0,4)==11,"All four light sections retained");
    require(record.extra->equals(decoded->extra.get()),"Entity, tile-tick and unknown NBT must remain intact");
    auto twice=load(save(*decoded));require(twice->extra->equals(decoded->extra.get()),"Owned array tags must survive record resaving");
    auto wrong=bytes;wrong[1]=7;rejects([&]{load(wrong);});wrong[1]=9;rejects([&]{load(wrong);});
    for(std::size_t cut=0;cut<bytes.size();cut+=97){std::vector<unsigned char> truncated(bytes.begin(),bytes.begin()+cut);rejects([&]{load(truncated);});CompressedTileStorage::tick();SparseDataStorage::tick();SparseLightStorage::tick();}
    bytes.pop_back();rejects([&]{load(bytes);});
    for(int i=0;i<3;++i){CompressedTileStorage::tick();SparseDataStorage::tick();SparseLightStorage::tick();}
    std::cout<<"Version-8 chunk sections, header, metadata and opaque entities passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
