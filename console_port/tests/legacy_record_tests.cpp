#include "PS3WorldStorage.h"
#include <iostream>
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
struct Input {ByteArrayInputStream bytes;DataInputStream data;Input(std::span<const unsigned char> s):bytes(byteArray(const_cast<unsigned char*>(s.data()),s.size())),data(&bytes){}~Input(){bytes.reset();}};
static std::vector<unsigned char> encode(CompoundTag* tag){ByteArrayOutputStream b;DataOutputStream d(&b);NbtIo::write(tag,&d);return {b.buf.data,b.buf.data+b.size()};}
static std::unique_ptr<CompoundTag> decode(std::span<const unsigned char> bytes){Input input(bytes);return std::unique_ptr<CompoundTag>(NbtIo::read(&input.data));}
static std::unique_ptr<console::ChunkRecord> load(std::span<const unsigned char> bytes){Input input(bytes);return console::ChunkRecord::readLegacy(&input.data);}
static std::vector<unsigned char> save(console::ChunkRecord& record){ByteArrayOutputStream bytes;DataOutputStream output(&bytes);record.writeLegacy(&output);return {bytes.buf.data,bytes.buf.data+bytes.size()};}
int main(){try{
    using console::ChunkRecord;
    for(unsigned height:{128u,256u}){
        std::vector<unsigned char> blocks(height*256),data(blocks.size()/2),sky(data.size()),light(data.size());std::array<unsigned char,256> heights{},biomes{};
        // Raw sections are concatenated 128-high x/z/y columns, not 256-high
        // interleaved columns. Distinct values expose swapped planes and nibbles.
        for(unsigned section=0;section<height/128;++section)for(unsigned x=0;x<16;++x)for(unsigned z=0;z<16;++z)for(unsigned y=0;y<128;++y){unsigned i=section*32768+(x*16+z)*128+y;blocks[i]=(x*7+z*11+y*3+section*29)%254;unsigned shift=(i&1)*4;data[i/2]|=((x+z+y+section)%16)<<shift;sky[i/2]|=((x*3+y+section)%16)<<shift;light[i/2]|=((z*5+y+section)%16)<<shift;}
        for(unsigned i=0;i<256;++i){heights[i]=i;biomes[i]=i%23;}
        CompoundTag root;auto level=std::make_unique<CompoundTag>();level->putInt(L"xPos",-33);level->putInt(L"zPos",32);level->putLong(L"LastUpdate",1ll<<48);level->putByteArray(L"Blocks",byteArray(blocks.data(),blocks.size()));level->putByteArray(L"Data",byteArray(data.data(),data.size()));level->putByteArray(L"SkyLight",byteArray(sky.data(),sky.size()));level->putByteArray(L"BlockLight",byteArray(light.data(),light.size()));level->putByteArray(L"HeightMap",byteArray(heights.data(),256));level->putByteArray(L"Biomes",byteArray(biomes.data(),256));level->putShort(L"TerrainPopulatedFlags",1022);level->putString(L"UnknownLevel",L"preserved");root.putString(L"UnknownRoot",L"also preserved");root.put(L"Level",level.get());auto* tag=level.release();
        auto fixture=encode(&root);auto record=load(fixture);require(record->x==-33 && record->z==32 && record->lastUpdate==(1ll<<48) && record->heightValues==heights && record->biomeValues==biomes && record->terrainPopulated==2046,"Legacy scalar, map and population migration");
        for(unsigned section=0;section<height/128;++section)for(unsigned x=0;x<16;++x)for(unsigned z=0;z<16;++z)for(unsigned y=0;y<128;++y){unsigned i=section*32768+(x*16+z)*128+y,shift=(i&1)*4;auto& b=section?record->upperBlocks:record->lowerBlocks;auto& d=section?record->upperData:record->lowerData;auto& s=section?record->upperSkyLight:record->lowerSkyLight;auto& l=section?record->upperBlockLight:record->lowerBlockLight;require(b->get(x,y,z)==blocks[i] && d->get(x,y,z)==((data[i/2]>>shift)&15) && s->get(x,y,z)==((sky[i/2]>>shift)&15) && l->get(x,y,z)==((light[i/2]>>shift)&15),"Every coordinate and nibble matches raw legacy arrays");}
        if(height==128)require(record->upperBlocks->get(1,1,1)==0 && record->upperData->get(1,1,1)==0 && record->upperSkyLight->get(1,1,1)==15 && record->upperBlockLight->get(1,1,1)==0,"Missing upper section uses original defaults");
        auto rewritten=save(*record);auto savedRoot=decode(rewritten);auto* savedLevel=savedRoot->getCompound(L"Level");require(savedLevel->getByteArray(L"Blocks").length==65536 && savedRoot->getString(L"UnknownRoot")==L"also preserved" && savedLevel->getString(L"UnknownLevel")==L"preserved","Legacy rewrite expands to current height and preserves unknown fields");
        auto again=load(rewritten);require(again->lowerBlocks->isSameAs(record->lowerBlocks.get()) && again->upperBlocks->isSameAs(record->upperBlocks.get()),"Legacy resave preserves compressed tile contents");
        tag->remove(L"Biomes");tag->putByte(L"TerrainPopulated",1);auto old=load(encode(&root));require(old->biomeValues[0]==255 && old->biomeValues[255]==255 && old->terrainPopulated==2046,"Missing biome sentinel and old bool precedence");tag->putByte(L"TerrainPopulated",0);require(load(encode(&root))->terrainPopulated==0,"Old false overrides newer flags");
        for(auto name:{L"Blocks",L"Data",L"SkyLight",L"BlockLight",L"HeightMap"}){auto changed=decode(fixture);changed->getCompound(L"Level")->putByteArray(name,byteArray());rejects([&]{load(encode(changed.get()));});}
        auto changed=decode(fixture);changed->getCompound(L"Level")->remove(L"xPos");rejects([&]{load(encode(changed.get()));});
        for(std::size_t cut=0;cut<fixture.size();cut+=997)rejects([&]{load(std::span(fixture).first(cut));});rejects([&]{load(std::span(fixture).first(fixture.size()-1));});
        // Archive version remains old after a chunk edit; original-version
        // controls the record family even when current-version is already 8.
        for(unsigned version=1;version<=7;++version){ConsoleSaveArchive archive;console::ConsoleRegionFile region;region.put(31,0,fixture);archive.put(L"r.-2.1.mcr",region.serialize());auto wire=archive.serialize();wire[9]=version;auto world=console::PS3WorldStorage::read(wire);auto loaded=world->chunk(0,-33,32);loaded->upperBlocks->set(4,100,5,6);world->putChunk(0,*loaded);auto output=world->serialize();require(output[9]==version && output[11]==8,"Container original version is preserved while editing legacy chunks");auto restored=console::PS3WorldStorage::read(output);require(restored->chunk(0,-33,32)->upperBlocks->get(4,100,5)==6,"Edited legacy chunk retains its format");}
    }
    // Current-version layouts 1 through 7 remain unchanged too. V1 has one
    // 136-byte file-table entry and a byte count rather than an entry count.
    for(unsigned version=1;version<=7;++version){
        ChunkRecord record;record.x=0;record.z=0;auto payload=save(record);console::ConsoleRegionFile region;region.put(0,0,payload);ConsoleSaveArchive archive;archive.put(L"r.0.0.mcr",region.serialize());auto wire=archive.serialize();wire[9]=version;wire[11]=version;
        if(version==1){wire.resize(wire.size()-8);SaveWire::write(wire,4,4,136,SaveByteOrder::Big);}
        auto old=console::PS3WorldStorage::read(wire);auto chunk=old->chunk(0,0,0);chunk->lowerBlocks->set(1,2,3,5);old->putChunk(0,*chunk);auto rewritten=old->serialize();require(rewritten[9]==version && rewritten[11]==version,"Legacy current and original versions survive edits");require(console::PS3WorldStorage::read(rewritten)->chunk(0,0,0)->lowerBlocks->get(1,2,3)==5,"Legacy file-table layouts retain edited chunk payloads");
    }
    CompoundTag missing;rejects([&]{load(encode(&missing));});
    std::cout<<"Legacy chunk arrays, defaults, migration, ownership and archive edits passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
