#include "WorldState.h"
#include "ChunkStorageCodec.h"
#include "LevelSettings.h"
#include "LevelType.h"
#include "RenderLightAccess.h"
#include "ConsoleLightmap.h"
#include "LiquidSurface.h"
#include "SkyColour.h"
#include "NaturalChunkDecoration.h"
#include <bit>
#include <limits>
namespace console {
namespace {
BiomeScale scaleFor(LevelData& data){
    if(data.getGenerator()==LevelType::lvl_flat)return BiomeScale::Normal;
    if(data.getGenerator()==LevelType::lvl_normal)return BiomeScale::Normal;
    if(data.getGenerator()==LevelType::lvl_largeBiomes)return BiomeScale::Large;
    if(data.getGenerator()==LevelType::lvl_normal_1_1)return BiomeScale::Legacy11;
    throw IoError("This client's world generator has not been ported yet");
}
void checkClientBlocks(const ChunkStorage& chunk,bool tutorial=false){
    for(auto id:chunk.blocks)if(!(tutorial?Tile::supported[id]:validBlock(id)))
        throw IoError("Save contains block "+std::to_string(id)+" not yet supported by the client");
}
std::uint64_t get64(std::span<const unsigned char> input,std::size_t& offset){
    if(offset>input.size() || input.size()-offset<8)throw IoError("Truncated prototype world save");
    std::uint64_t result=0;for(int i=0;i<8;++i)result|=std::uint64_t(input[offset++])<<(i*8);return result;
}
std::uint64_t checksum(std::span<const unsigned char> data){std::uint64_t hash=14695981039346656037ull;for(auto value:data)hash=(hash^value)*1099511628211ull;return hash;}
wchar_t decorationMarker[]=L"console_port.needsNaturalDecoration";
bool needsDecoration(const ChunkRecord& record){return record.extra && record.extra->getBoolean(decorationMarker);}
void markDecoration(ChunkRecord& record,bool pending){
    if(pending)record.extra->putBoolean(decorationMarker,true);
    else record.extra->remove(decorationMarker);
}
void appendDungeonTiles(ChunkRecord& record,const std::vector<DungeonTile>& tiles){
    if(tiles.empty())return;
    if(!record.extra)record.extra=std::make_unique<CompoundTag>();
    auto* list=dynamic_cast<TagList*>(record.extra->get(L"TileEntities"));
    if(!list){record.extra->put(L"TileEntities",new TagList());list=record.extra->getList(L"TileEntities");}
    for(const auto& tile:tiles){
        bool exists=false;
        for(int i=0;i<list->size();++i)if(auto* old=dynamic_cast<CompoundTag*>(list->get(i)))
            if(old->getInt(L"x")==tile.x && old->getInt(L"y")==tile.y && old->getInt(L"z")==tile.z)exists=true;
        if(exists)continue;
        auto tag=std::make_unique<CompoundTag>();
        tag->putInt(L"x",tile.x);tag->putInt(L"y",tile.y);tag->putInt(L"z",tile.z);
        if(tile.spawner){
            tag->putString(L"id",L"MobSpawner");tag->putString(L"EntityId",tile.entityId);
            tag->putShort(L"Delay",20);tag->putShort(L"MinSpawnDelay",200);
            tag->putShort(L"MaxSpawnDelay",800);tag->putShort(L"SpawnCount",4);
        }else{
            tag->putString(L"id",L"Chest");auto items=std::make_unique<TagList>();
            for(const auto& item:tile.items){
                auto stack=std::make_unique<CompoundTag>();stack->putByte(L"Slot",item.slot);
                stack->putShort(L"id",item.id);stack->putByte(L"Count",item.count);
                stack->putShort(L"Damage",item.damage);
                if(item.enchantmentId>=0){
                    auto stackTag=std::make_unique<CompoundTag>();
                    auto enchants=std::make_unique<TagList>();
                    auto enchant=std::make_unique<CompoundTag>();
                    enchant->putShort(L"id",item.enchantmentId);
                    enchant->putShort(L"lvl",item.enchantmentLevel);
                    enchants->add(enchant.get());enchant.release();
                    stackTag->put(L"StoredEnchantments",enchants.get());enchants.release();
                    stack->put(L"tag",stackTag.get());stackTag.release();
                }
                items->add(stack.get());stack.release();
            }
            tag->put(L"Items",items.get());items.release();
        }
        list->add(tag.get());tag.release();
    }
}
}
World::State::State(std::int64_t seed):archive(std::make_unique<PS3WorldStorage>()),fluidRandom(seed),entityRandom(seed^0x5deece66){
    GameType::staticCtor();LevelType::staticCtor();
    LevelSettings settings(seed,GameType::CREATIVE,true,false,true,LevelType::lvl_normal,54,3);
    metadata=std::make_unique<LevelData>(&settings,L"Console World");
    for(int x=-width/32;x<width/32;++x)for(int z=-depth/32;z<depth/32;++z)
        region.insert(x,z,std::make_unique<ChunkStorage>());
    configureLight(seed);
}
void World::State::configureLight(std::int64_t seed){
    lightLevel=std::make_unique<::Level>(seed,metadata->getXZSize(),scaleFor(*metadata));lightLevel->setBlockAccess(region);
    if(metadata->getGenerator()==LevelType::lvl_flat)lightLevel->getBiomeSource()->setFixedBiome(1);
}
void World::State::ensureLighting(std::int64_t seed){
    if(region.hasPreparedLight())return;
    lightDirty=true;
    std::unique_ptr<ChunkGenerator> generator;
    bool flat=metadata->getGenerator()==LevelType::lvl_flat;
    if(terrain && !flat)generator=std::make_unique<ChunkGenerator>(seed,metadata->getXZSize(),scaleFor(*metadata));
    // The visible central region has a two-chunk halo for the original 17-block
    // propagation checks. Existing saved halo chunks retain their opaque fields.
    for(int x=originX/16-width/32-2;x<originX/16+width/32+2;++x)for(int z=originZ/16-depth/32-2;z<originZ/16+depth/32+2;++z){
        if(region.hasChunk(x,z))continue;
        auto record=archive->chunk(0,x,z);
        if(record){auto restored=ChunkStorageCodec::restore(*record);region.insert(x,z,std::move(restored.storage));if(needsDecoration(*record))undecorated.insert({x,z});records[{x,z}]=std::move(record);}
        else if(archivedTutorial){
            auto source=archivedTutorial->chunk(x,z);
            if(source){auto restored=ChunkStorageCodec::restore(*source);
                region.insert(x,z,std::move(restored.storage));records[{x,z}]=std::move(source);}
            else region.insert(x,z,std::make_unique<ChunkStorage>());
        }else if(terrain && !flat){
            auto generated=generator->generate(x,z);
            region.insert(x,z,std::make_unique<ChunkStorage>(generated));
            rawBase[{x,z}]=std::make_unique<ChunkStorage>(generated);
            undecorated.insert({x,z});
        }else region.insert(x,z,terrain?std::make_unique<ChunkStorage>(generateFlatChunk()):std::make_unique<ChunkStorage>());
    }
    region.initializeLight(*lightLevel);
    lightDirty=false;
}
World::World():state(std::make_unique<State>(0)){}
World::~World()=default;
std::vector<std::uint8_t> World::blockSnapshot()const{
    std::vector<std::uint8_t> result(width*height*depth);
    for(int z=0;z<depth;++z)for(int x=0;x<width;++x)
        std::copy_n(state->chunk(x+originX(),z+originZ()).blocks.data()+((x&15)*16+(z&15))*height,height,result.data()+(z*width+x)*height);
    return result;
}
void World::generate(std::int64_t value,bool flat){
    auto next=std::make_unique<State>(value);next->terrain=true;ChunkGenerator generator(value);
    if(flat){next->metadata->setGenerator(LevelType::lvl_flat);next->configureLight(value);}
    const int margin=flat?0:2;
    for(int x=-width/32-margin;x<width/32+margin;++x)for(int z=-depth/32-margin;z<depth/32+margin;++z){
        if(flat)next->region.insert(x,z,std::make_unique<ChunkStorage>(generateFlatChunk()));
        else{
            auto generated=generator.generate(x,z);
            next->region.insert(x,z,std::make_unique<ChunkStorage>(generated));
            next->rawBase[{x,z}]=std::make_unique<ChunkStorage>(generated);
            next->undecorated.insert({x,z});
        }
    }
    if(!flat)for(int x=-width/32;x<width/32;++x)for(int z=-depth/32;z<depth/32;++z)
        next->decorateNatural(x,z);
    next->metadata->setInitialized(true);
    state.swap(next);seed=value;++revision;
    activateFluidChunks();
    const auto start=spawn();state->metadata->setSpawn(int(start.x)-width/2,int(start.y),int(start.z)-depth/2);
}
void World::State::decorateNatural(int x,int z,std::map<std::pair<int,int>,std::unique_ptr<ChunkStorage>>* incoming){
    const auto source=[&](int cx,int cz)->const ChunkStorage&{return *rawBase.at({cx,cz});};
    std::function<bool(ChunkStorage&,int,int)> applyRules;
    if(tutorial)applyRules=[&](ChunkStorage& chunk,int sx,int sz){return tutorial->apply(chunk,sx,sz);};
    std::vector<DungeonTile> dungeonTiles;
    auto result=decorateNaturalColumn(metadata->getSeed(),metadata->getXZSize(),scaleFor(*metadata),x,z,source,applyRules,&dungeonTiles);
    auto key=std::pair{x,z};
    if((tutorial || !dungeonTiles.empty()) && !records.contains(key)){
        auto& targetRecords=incoming?pending->records:records;
        if(!targetRecords.contains(key)){
            auto record=std::make_unique<ChunkRecord>();record->x=x;record->z=z;
            if(tutorial)record->extra=tutorial->tagsForChunk(x,z);
            targetRecords[key]=std::move(record);
        }
    }
    if(!dungeonTiles.empty()){
        auto* record=incoming && pending->records.contains(key)?pending->records.at(key).get():records.at(key).get();
        appendDungeonTiles(*record,dungeonTiles);
    }
    if(incoming){
        if(!incoming->contains(key))pending->decoratedExisting.insert(key);
        (*incoming)[key]=std::move(result);
        pending->undecorated.erase(key);
        if(auto it=pending->records.find(key);it!=pending->records.end())markDecoration(*it->second,false);
    }else{
        region.insert(x,z,std::move(result));undecorated.erase(key);
        if(auto it=records.find(key);it!=records.end())markDecoration(*it->second,false);
    }
}
void World::generateTutorial(const std::filesystem::path& assets){
    auto package=NativeSaveFile::read(assets/"Tutorial.pck");
    auto next=std::make_unique<State>(tutorialSeed);next->tutorial=std::make_unique<TutorialSchematics>(package);
    next->archive->putEntry(L"console_port.tutorial.pck",package);next->terrain=true;
    next->metadata->setSpawn(tutorialSpawn[0],tutorialSpawn[1],tutorialSpawn[2]);next->metadata->setLevelName(L"Tutorial");next->metadata->setInitialized(true);
    next->originX=Mth::intFloorDiv(tutorialSpawn[0],16)*16;next->originZ=Mth::intFloorDiv(tutorialSpawn[2],16)*16;
    next->region.chunks.clear();ChunkGenerator generator(tutorialSeed);
    for(int x=next->originX/16-width/32-2;x<next->originX/16+width/32+2;++x)
        for(int z=next->originZ/16-depth/32-2;z<next->originZ/16+depth/32+2;++z){
            auto generated=generator.generate(x,z);
            next->region.insert(x,z,std::make_unique<ChunkStorage>(generated));
            next->rawBase[{x,z}]=std::make_unique<ChunkStorage>(generated);
            next->undecorated.insert({x,z});
        }
    for(int x=next->originX/16-width/32;x<next->originX/16+width/32;++x)
        for(int z=next->originZ/16-depth/32;z<next->originZ/16+depth/32;++z)
            next->decorateNatural(x,z);
    next->ensureLighting(tutorialSeed);state.swap(next);seed=tutorialSeed;++revision;
    for(const auto& [key,record]:state->records)loadEntities(*record);
    activateFluidChunks();
}
void World::generateArchivedTutorial(const std::filesystem::path& assets){
    auto wrapped=NativeSaveFile::read(assets/"Tutorial");
    auto next=std::make_unique<State>(0);
    next->archivedTutorial=std::make_unique<TutorialWorldSource>(wrapped);
    next->metadata=next->archivedTutorial->metadata();
    const auto originalSeed=next->metadata->getSeed();
    next->metadata->setGameType(GameType::CREATIVE);
    next->metadata->setLevelName(L"Classic Tutorial");
    next->metadata->setInitialized(true);
    next->archive->putEntry(L"console_port.tutorial.base",wrapped);
    next->originX=Mth::intFloorDiv(next->metadata->getXSpawn(),16)*16;
    next->originZ=Mth::intFloorDiv(next->metadata->getZSpawn(),16)*16;
    next->region.chunks.clear();
    for(int x=next->originX/16-width/32-2;x<next->originX/16+width/32+2;++x)
        for(int z=next->originZ/16-depth/32-2;z<next->originZ/16+depth/32+2;++z){
            auto record=next->archivedTutorial->chunk(x,z);
            if(!record)throw IoError("Archived tutorial is missing a spawn-area chunk");
            auto restored=ChunkStorageCodec::restore(*record);
            checkClientBlocks(*restored.storage,true);
            next->region.insert(x,z,std::move(restored.storage));
            next->records[{x,z}]=std::move(record);
        }
    next->configureLight(originalSeed);
    next->ensureLighting(originalSeed);
    state.swap(next);seed=originalSeed;++revision;
    for(const auto& [key,record]:state->records)loadEntities(*record);
    activateFluidChunks();
}
bool World::isTutorial()const{return bool(state->tutorial || state->archivedTutorial);}
bool World::isFlat()const{return state->metadata->getGenerator()==LevelType::lvl_flat;}
Block World::get(int x,int y,int z)const{
    if(!inside(x,y,z))return Air;
    return static_cast<Block>(state->chunk(x,z).blocks[((x&15)*16+(z&15))*height+y]);
}
int World::getData(int x,int y,int z)const{return inside(x,y,z)?state->chunk(x,z).metadata.get(x&15,y,z&15):0;}
bool World::set(int x,int y,int z,Block tile){
    if(!inside(x,y,z) || !validBlock(tile) || get(x,y,z)==tile)return false;
    const bool lighting=state->pending && state->pending->phase==State::Pending::Phase::Lighting && !state->region.hasPreparedLight();
    if(!lighting)state->ensureLighting(seed);
    state->lightDirty=true;
    bool changed=state->lightLevel->setTileAndDataNoUpdate(x-width/2,y,z-depth/2,tile,0);
    if(lighting){if(changed)state->pending->restartLighting=true;}
    else state->lightDirty=false;
    if(changed){state->undecorated.erase({Mth::intFloorDiv(x-width/2,16),Mth::intFloorDiv(z-depth/2,16)});++revision;}return changed;
}
bool World::setData(int x,int y,int z,int data){
    if(!inside(x,y,z) || data<0 || data>15 || getData(x,y,z)==data)return false;
    const bool lighting=state->pending && state->pending->phase==State::Pending::Phase::Lighting && !state->region.hasPreparedLight();
    if(!lighting)state->ensureLighting(seed);
    state->lightDirty=true;
    bool changed=state->lightLevel->setTileAndDataNoUpdate(x-width/2,y,z-depth/2,get(x,y,z),data);
    if(lighting){if(changed)state->pending->restartLighting=true;}
    else state->lightDirty=false;
    if(changed){state->undecorated.erase({Mth::intFloorDiv(x-width/2,16),Mth::intFloorDiv(z-depth/2,16)});++revision;}return changed;
}
int World::skyLight(int x,int y,int z)const{if(!inside(x,y,z))return 0;if(state->lightDirty)state->ensureLighting(seed);return state->chunk(x,z).skyLight.get(x&15,y,z&15);}
int World::blockLight(int x,int y,int z)const{if(!inside(x,y,z))return 0;if(state->lightDirty)state->ensureLighting(seed);return state->chunk(x,z).blockLight.get(x&15,y,z&15);}
void World::setName(const std::string& name){
    if(name.empty() || name.size()>25)throw std::invalid_argument("World name must contain 1 to 25 characters");
    for(unsigned char c:name)if(c<32 || c>=127)throw std::invalid_argument("Unsupported world name character");
    state->metadata->setLevelName(std::wstring(name.begin(),name.end()));
}
std::int64_t World::time()const{return state->metadata->getTime();}
float World::rainLevel()const{return state->metadata->isRaining()?1.f:0.f;}
float World::thunderLevel()const{return state->metadata->isThundering()?rainLevel():0;}
void World::tickTime(){
    // ServerLevel advances one stored world tick; define its console wrap explicitly.
    state->metadata->setTime(std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(time())+1));
    tickFluids();
    tickFurnaces();
    tickBrewingStands();
    tickPlayerEffects();
    tickEntities();
}
std::array<float,3> World::skyColour(int x,int z)const{
    if(x<originX() || x>=originX()+width || z<originZ() || z>=originZ()+depth)throw std::out_of_range("Sky sample outside client world");
    int biome=state->chunk(x,z).biomes[(z&15)*16+(x&15)];
    return consoleSkyColour(biome,time(),rainLevel(),state->metadata->isThundering()?rainLevel():0);
}
std::array<int,9> World::neighboringBiomes(int x,int z)const{
    if(x<originX() || x>=originX()+width || z<originZ() || z>=originZ()+depth)throw std::out_of_range("Biome sample outside client world");
    if(!state->region.hasPreparedLight())state->ensureLighting(seed);
    std::array<int,9> result;
    for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
        int xx=x+dx-width/2,zz=z+dz-depth/2;
        auto& chunk=*state->region.chunks.at({Mth::intFloorDiv(xx,16),Mth::intFloorDiv(zz,16)});
        result[(dz+1)*3+dx+1]=chunk.biomes[(zz&15)*16+(xx&15)];
    }
    return result;
}
float World::skyDarken()const {
    return consoleSkyDarken(state->metadata->getTime(),state->metadata->isRaining()?1.f:0.f,
        state->metadata->isThundering() && state->metadata->isRaining()?1.f:0.f);
}
int World::renderLight(int x,int y,int z,bool liquid)const {
    // Terrain faces may sample just outside the visible region. Prepare the
    // original light neighborhood instead of producing a dark border seam.
    if(x < originX()-1 || x > originX()+width || z < originZ()-1 || z > originZ()+depth || y < -1 || y > height)
        throw std::out_of_range("Render light sample outside the visible region boundary");
    state->ensureLighting(seed);
    struct Access final:RenderLightAccess {
        GenerationRegion& region;
        explicit Access(GenerationRegion& value):region(value) {}
        bool hasCeiling()const override{return false;}
        bool hasChunk(int x,int z)const override{return region.hasChunk(x,z);}
        int tile(int x,int y,int z)const override{return region.getTile(x,y,z);}
        bool propagates(int id)const override {
            // Tile::staticCtor: slabs/farmland, translucent materials and
            // zero-opacity blocks use the brightest of the five neighbors.
            // Air has no registered Tile, so the original initialization loop
            // leaves propagate[0] false even though its opacity is zero.
            return id!=0 && (id==44 || id==60 || !Tile::materialFor(id)->blocksLight() || Tile::lightBlockFor(id)==0);
        }
        int brightness(LightLayer::variety layer,int x,int y,int z)const override {
            if(!hasChunk(Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16)))return int(layer);
            return region.getLight(layer,x,std::clamp(y,0,255),z);
        }
        int storedLight(LightLayer::variety layer,int x,int y,int z)const override {
            return y<0 || y>=256?0:region.getLight(layer,x,y,z);
        }
    } access(state->region);
    x-=width/2;z-=depth/2;
    int id=access.tile(x,y,z);
    return liquid?sampleConsoleLiquidLight(access,x,y,z):sampleConsoleLight(access,x,y,z,Tile::lightEmission[id],id);
}
int World::originX()const{return state->originX;}
int World::originZ()const{return state->originZ;}
int World::worldMin()const{return width/2-state->metadata->getXZSize()*8;}
int World::worldMax()const{return width/2+state->metadata->getXZSize()*8;}
bool World::streaming()const{return bool(state->pending);}
std::size_t World::residentChunks()const{return state->region.chunks.size();}
bool World::streamAround(Vec3 player,int chunkBudget){
    if(!std::isfinite(player.x) || !std::isfinite(player.z) || chunkBudget<1 || chunkBudget>144)
        throw std::invalid_argument("Invalid chunk streaming request");
    const auto target=[&](double coordinate,int current){
        coordinate=std::clamp(coordinate,double(worldMin()),double(worldMax()-1));
        if(std::abs(coordinate-(current+width/2))<=16)return current;
        return std::clamp(int(std::floor((coordinate-width/2+8)/16))*16,worldMin(),worldMax()-width);
    };
    if(!state->pending){
        int ox=target(player.x,originX()),oz=target(player.z,originZ());
        if(ox==originX() && oz==originZ())return false;
        auto pending=std::make_unique<State::Pending>();pending->x=ox;pending->z=oz;
        for(int x=ox/16-width/32-2;x<ox/16+width/32+2;++x)
            for(int z=oz/16-depth/32-2;z<oz/16+depth/32+2;++z)
                if(!state->region.hasChunk(x,z))pending->missing.push_back({x,z});
        state->pending=std::move(pending);
    }
    try{
        auto& pending=*state->pending;
        if(pending.phase==State::Pending::Phase::Lighting){
            if(pending.restartLighting){state->region.beginLightInitialization();pending.restartLighting=false;}
            if(!state->region.hasPreparedLight() &&
               !state->region.stepLightInitialization(*state->lightLevel,std::max(16,chunkBudget*12)))return false;
            state->lightDirty=false;state->pending.reset();++revision;return true;
        }
        if(pending.phase==State::Pending::Phase::Chunks)
        for(int i=0;i<chunkBudget && !pending.missing.empty();++i){
            auto key=pending.missing.back();
            auto record=state->archive->chunk(0,key.first,key.second);
            std::unique_ptr<ChunkStorage> chunk;
            if(record){auto restored=ChunkStorageCodec::restore(*record);chunk=std::move(restored.storage);if(needsDecoration(*record))pending.undecorated.insert(key);}
            else if(state->archivedTutorial){
                record=state->archivedTutorial->chunk(key.first,key.second);
                if(record){auto restored=ChunkStorageCodec::restore(*record);chunk=std::move(restored.storage);}
                else chunk=std::make_unique<ChunkStorage>();
            }
            else if(isFlat())chunk=std::make_unique<ChunkStorage>(generateFlatChunk());
            else {
                if(!state->generator)state->generator=std::make_unique<ChunkGenerator>(seed,state->metadata->getXZSize(),scaleFor(*state->metadata));
                auto generated=state->generator->generate(key.first,key.second);
                chunk=std::make_unique<ChunkStorage>(generated);
                state->rawBase[key]=std::make_unique<ChunkStorage>(generated);
                pending.undecorated.insert(key);
            }
            if(state->tutorial && !record){
                state->tutorial->apply(*chunk,key.first,key.second);chunk->recalculateHeightmap();
                record=std::make_unique<ChunkRecord>();record->x=key.first;record->z=key.second;record->extra=state->tutorial->tagsForChunk(key.first,key.second);
            }
            checkClientBlocks(*chunk,isTutorial());
            pending.chunks[key]=std::move(chunk);
            if(record)pending.records[key]=std::move(record);
            pending.missing.pop_back();
        }
        if(pending.phase==State::Pending::Phase::Chunks){
            if(!pending.missing.empty())return false;
            for(int x=pending.x/16-width/32;x<pending.x/16+width/32;++x)
                for(int z=pending.z/16-depth/32;z<pending.z/16+depth/32;++z){
                    auto key=std::pair{x,z};
                    if(state->undecorated.contains(key) || pending.undecorated.contains(key))
                        pending.decorationTargets.push_back(key);
                }
            std::set<std::pair<int,int>> needed;
            for(const auto& [x,z]:pending.decorationTargets)
                for(int bx=x-2;bx<=x+2;++bx)for(int bz=z-2;bz<=z+2;++bz)
                    if(!state->rawBase.contains({bx,bz}))needed.insert({bx,bz});
            pending.baseMissing.assign(needed.begin(),needed.end());
            pending.phase=State::Pending::Phase::Base;
            return false;
        }
        if(pending.phase==State::Pending::Phase::Base){
            if(!pending.baseMissing.empty()){
                if(!state->generator)state->generator=std::make_unique<ChunkGenerator>(seed,state->metadata->getXZSize(),scaleFor(*state->metadata));
                for(int i=0;i<chunkBudget && !pending.baseMissing.empty();++i){
                    auto key=pending.baseMissing.back();pending.baseMissing.pop_back();
                    state->rawBase[key]=std::make_unique<ChunkStorage>(state->generator->generate(key.first,key.second));
                }
                return false;
            }
            pending.phase=State::Pending::Phase::Decorating;
            return false;
        }
        if(pending.phase==State::Pending::Phase::Decorating){
            // At most one detached natural-feature pass per call. This never
            // mutates retained or imported neighbors and leaves the old view
            // playable until the incoming region is complete.
            if(pending.decorationIndex<pending.decorationTargets.size()){
                const auto key=pending.decorationTargets[pending.decorationIndex++];
                state->decorateNatural(key.first,key.second,&pending.chunks);
                return false;
            }
            const auto retained=[&](std::pair<int,int> key){
                return key.first>=pending.x/16-width/32-2 && key.first<pending.x/16+width/32+2 &&
                    key.second>=pending.z/16-depth/32-2 && key.second<pending.z/16+depth/32+2;
            };
            for(const auto& [key,chunk]:state->region.chunks)if(!retained(key))pending.outgoing.push_back(key);
            pending.captureRevision=revision;
            pending.phase=State::Pending::Phase::Evicting;
            return false;
        }
        const auto retained=[&](std::pair<int,int> key){
            return key.first>=pending.x/16-width/32-2 && key.first<pending.x/16+width/32+2 &&
                key.second>=pending.z/16-depth/32-2 && key.second<pending.z/16+depth/32+2;
        };
        // Archive outgoing chunks in bounded batches. A block edit during this
        // phase restarts the capture so the final archive contains that edit.
        if(revision!=pending.captureRevision){pending.captureRevision=revision;pending.outgoingIndex=0;}
        for(int i=0;i<chunkBudget*4 && pending.outgoingIndex<pending.outgoing.size();++i){
            const auto key=pending.outgoing[pending.outgoingIndex];
            const auto& chunk=*state->region.chunks.at(key);
            ChunkRecord fresh;fresh.x=key.first;fresh.z=key.second;
            auto it=state->records.find(key);
            auto record=ChunkStorageCodec::capture(chunk,it==state->records.end()?fresh:*it->second);
            saveFluidTicks(*record,true);
            saveEntities(*record,true);
            markDecoration(*record,state->undecorated.contains(key));
            record->lastUpdate=time();state->archive->putChunk(0,*record);
            ++pending.outgoingIndex;
        }
        if(pending.outgoingIndex<pending.outgoing.size())return false;
        for(auto it=state->region.chunks.begin();it!=state->region.chunks.end();){
            if(retained(it->first)){++it;continue;}
            state->records.erase(it->first);state->undecorated.erase(it->first);it=state->region.chunks.erase(it);
        }
        for(auto& [key,chunk]:pending.chunks)state->region.insert(key.first,key.second,std::move(chunk));
        for(const auto& key:pending.decoratedExisting){
            state->undecorated.erase(key);
            if(auto it=state->records.find(key);it!=state->records.end())markDecoration(*it->second,false);
        }
        for(auto it=state->rawBase.begin();it!=state->rawBase.end();){
            if(retained(it->first))++it;else it=state->rawBase.erase(it);
        }
        state->undecorated.insert(pending.undecorated.begin(),pending.undecorated.end());
        for(auto& [key,record]:pending.records)state->records[key]=std::move(record);
        const int previousX=state->originX,previousZ=state->originZ;
        state->originX=pending.x;state->originZ=pending.z;
        state->region.changedLightColumns.clear();state->lightDirty=true;
        for(int cx=state->originX/16-4;cx<state->originX/16+4;++cx)
            for(int cz=state->originZ/16-4;cz<state->originZ/16+4;++cz){
                const int clientX=cx*16+width/2,clientZ=cz*16+depth/2;
                if(clientX>=previousX && clientX<previousX+width &&
                   clientZ>=previousZ && clientZ<previousZ+depth)continue;
                if(auto it=state->records.find({cx,cz});it!=state->records.end())loadFluidTicks(*it->second);
                activateFluidChunk(cx,cz);
            }
        // A record can have been resident in the old lighting halo while its
        // entities were outside the visible window. Activate every now-visible
        // record; loadEntities identifies already-active source entries.
        for(const auto& [key,record]:state->records)
            if(key.first>=state->originX/16-4 && key.first<state->originX/16+4 &&
               key.second>=state->originZ/16-4 && key.second<state->originZ/16+4)
                loadEntities(*record);
        // Keep the new window playable while the two original light passes run
        // over a bounded number of chunks each frame. The renderer keeps its
        // previous mesh until lighting completes.
        pending.phase=State::Pending::Phase::Lighting;
        state->region.beginLightInitialization();
        return false;
    }catch(...){
        state->pending.reset();
        for(auto it=state->rawBase.begin();it!=state->rawBase.end();){
            if(state->region.hasChunk(it->first.first,it->first.second))++it;
            else it=state->rawBase.erase(it);
        }
        throw;
    }
}
void World::save(const std::filesystem::path& path){
    if(state->lightDirty)state->ensureLighting(seed);
    auto candidate=PS3WorldStorage::read(state->archive->serialize());
    auto metadata=std::make_unique<LevelData>(state->metadata.get());metadata->setSeed(seed);candidate->putMetadata(*metadata);
    for(const auto& [position,chunk]:state->region.chunks){
        ChunkRecord fresh;fresh.x=position.first;fresh.z=position.second;
        auto it=state->records.find(position);const auto& context=it==state->records.end()?fresh:*it->second;
        auto record=ChunkStorageCodec::capture(*chunk,context);record->lastUpdate=metadata->getTime();
        saveFluidTicks(*record,false);
        saveEntities(*record,false);
        markDecoration(*record,state->undecorated.contains(position));candidate->putChunk(0,*record);
    }
    // Player::addAdditionalSaveData stores the personal 27-slot chest under
    // EnderItems, separate from the carried Items and every block entity.
    auto playerItems=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(state->inventory->copy()));
    playerItems->putShort(L"Health",state->playerHurt.health);
    playerItems->putShort(L"HurtTime",state->playerHurt.hurtTicks);
    playerItems->putShort(L"DeathTime",state->playerHurt.deathTicks);
    state->playerFood.addAdditonalSaveData(playerItems.get());
    state->playerExperience.addAdditionalSaveData(playerItems.get());
    if(!state->playerEffects.empty()){
        auto effects=std::make_unique<TagList>();
        for(const auto& effect:state->playerEffects){
            auto tag=std::make_unique<CompoundTag>();
            tag->putByte(L"Id",effect.id);tag->putByte(L"Amplifier",effect.amplifier);
            tag->putInt(L"Duration",effect.duration);effects->add(tag.get());tag.release();
        }
        playerItems->put(L"ActiveEffects",effects.get());effects.release();
    }else playerItems->remove(L"ActiveEffects");
    if(auto* ender=dynamic_cast<TagList*>(state->enderInventory->get(L"Items"))){
        std::unique_ptr<Tag> copy(ender->copy());playerItems->put(L"EnderItems",copy.get());copy.release();
    }else playerItems->put(L"EnderItems",new TagList());
    ByteArrayOutputStream inventoryBytes;DataOutputStream inventoryOutput(&inventoryBytes);
    NbtIo::write(playerItems.get(),&inventoryOutput);auto inventoryData=inventoryBytes.toByteArray();
    std::unique_ptr<unsigned char[]> inventoryOwner(inventoryData.data);
    candidate->putEntry(L"console_port.inventory.dat",std::span<const unsigned char>(inventoryData.data,inventoryData.length));

    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    candidate->writeFile(path);
    state->archive.swap(candidate);state->metadata.swap(metadata);
    for(auto& [position,chunk]:state->region.chunks)chunk->unsaved=false;
}
bool World::load(const std::filesystem::path& path){
    if(!std::filesystem::exists(path))return false;
    auto bytes=NativeSaveFile::read(path);
    auto next=std::make_unique<State>(0);std::int64_t loadedSeed=0;
    if(std::memcmp(bytes.data(),"MCPORT01",8)==0){
        std::size_t offset=8;loadedSeed=std::bit_cast<std::int64_t>(get64(bytes,offset));auto length=get64(bytes,offset),expected=get64(bytes,offset);
        const auto oldHeight=length/(width*depth);
        if((oldHeight!=96 && oldHeight!=128 && oldHeight!=256) || length!=width*depth*oldHeight)throw IoError("Invalid prototype dimensions");
        if(bytes.size()-offset!=length)throw IoError("Prototype save has a truncated or trailing payload");
        auto blocks=std::span<const unsigned char>(bytes).subspan(offset);
        if(checksum(blocks)!=expected || !std::all_of(blocks.begin(),blocks.end(),validBlock))throw IoError("Prototype save is damaged");
        for(int x=0;x<width;++x)for(int z=0;z<depth;++z)
            std::copy_n(blocks.data()+(z*width+x)*oldHeight,oldHeight,next->chunk(x,z).blocks.data()+((x&15)*16+(z&15))*height);
        for(auto& [position,chunk]:next->region.chunks)chunk->recalculateHeightmap();
        next->metadata->setSeed(loadedSeed);next->metadata->setInitialized(true);
    }else{
        next->archive=PS3WorldStorage::read(bytes);next->metadata=next->archive->metadata();
        if(next->archive->containsEntry(L"console_port.inventory.dat")){
            auto data=next->archive->entry(L"console_port.inventory.dat");
            ByteArrayInputStream input(byteArray(const_cast<unsigned char*>(data.data()),data.size()));
            struct Detach{ByteArrayInputStream& stream;~Detach(){stream.reset();}}detach{input};
            DataInputStream stream(&input);next->inventory.reset(NbtIo::read(&stream));
            if(input.read()!=-1)throw IoError("Trailing carried inventory data");
            containerItems(*next->inventory,36);
            if(next->inventory->contains(L"Health"))
                next->playerHurt.health=std::clamp(int(next->inventory->getShort(L"Health")),0,20);
            next->playerHurt.hurtTicks=std::max(0,int(next->inventory->getShort(L"HurtTime")));
            next->playerHurt.deathTicks=std::max(0,int(next->inventory->getShort(L"DeathTime")));
            next->playerFood.readAdditionalSaveData(next->inventory.get());
            if(next->inventory->contains(L"XpP") || next->inventory->contains(L"XpLevel") ||
               next->inventory->contains(L"XpTotal"))
                next->playerExperience.readAdditionalSaveData(next->inventory.get());
            if(next->inventory->contains(L"ActiveEffects")){
                auto* effects=dynamic_cast<TagList*>(next->inventory->get(L"ActiveEffects"));
                if(!effects)throw IoError("Invalid player effect list");
                for(int i=0;i<effects->size();++i){
                    auto* tag=dynamic_cast<CompoundTag*>(effects->get(i));
                    if(!tag || !tag->contains(L"Id") || !tag->contains(L"Amplifier") ||
                       !tag->contains(L"Duration"))throw IoError("Invalid player effect entry");
                    PotionEffect effect{int(static_cast<unsigned char>(tag->getByte(L"Id"))),
                        tag->getInt(L"Duration"),
                        int(static_cast<unsigned char>(tag->getByte(L"Amplifier")))};
                    if(effect.id<1 || effect.id>19 || effect.duration<=0)
                        throw IoError("Invalid player effect values");
                    next->playerEffects.push_back(effect);
                }
            }
            if(next->inventory->contains(L"EnderItems")){
                auto* items=dynamic_cast<TagList*>(next->inventory->get(L"EnderItems"));
                if(!items)throw IoError("Invalid ender chest item list");
                std::unique_ptr<Tag> copy(items->copy());next->enderInventory->put(L"Items",copy.get());copy.release();
                containerItems(*next->enderInventory,27);
                next->inventory->remove(L"EnderItems");
            }
        }
        if(!next->metadata)throw IoError("Save has no level metadata");
        if(next->metadata->getGameType()!=GameType::CREATIVE)throw IoError("This client's survival/adventure simulation has not been ported yet");
        if(next->metadata->getXZSize()!=54)throw IoError("This client currently supports the original 54-chunk world size");
        loadedSeed=next->metadata->getSeed();next->terrain=true;bool repair=false;
        if(next->archive->containsEntry(L"console_port.tutorial.base")){
            next->archivedTutorial=std::make_unique<TutorialWorldSource>(
                next->archive->entry(L"console_port.tutorial.base"));
            if(next->archivedTutorial->metadata()->getSeed()!=loadedSeed)
                throw IoError("Archived tutorial seed mismatch");
            next->terrain=false;
            next->originX=Mth::intFloorDiv(next->metadata->getXSpawn(),16)*16;
            next->originZ=Mth::intFloorDiv(next->metadata->getZSpawn(),16)*16;
            next->region.chunks.clear();
        }
        if(next->archive->containsEntry(L"console_port.tutorial.pck")){
            next->tutorial=std::make_unique<TutorialSchematics>(next->archive->entry(L"console_port.tutorial.pck"));
            if(loadedSeed!=tutorialSeed)throw IoError("Tutorial save seed mismatch");
            next->originX=Mth::intFloorDiv(tutorialSpawn[0],16)*16;next->originZ=Mth::intFloorDiv(tutorialSpawn[2],16)*16;
            next->region.chunks.clear();
        }
        for(int x=next->originX/16-width/32;x<next->originX/16+width/32;++x)for(int z=next->originZ/16-depth/32;z<next->originZ/16+depth/32;++z){
            auto record=next->archive->chunk(0,x,z);
            if(!record && next->archivedTutorial)record=next->archivedTutorial->chunk(x,z);
            if(!record)throw IoError("Save is missing a chunk in the current client region");
            auto restored=ChunkStorageCodec::restore(*record);checkClientBlocks(*restored.storage,bool(next->tutorial || next->archivedTutorial));repair|=restored.needsLightRebuild;
            next->region.insert(x,z,std::move(restored.storage));if(needsDecoration(*record))next->undecorated.insert({x,z});next->records[{x,z}]=std::move(record);
        }
        // Stored light is retained until an edit requires a full prepared halo.
        // Loading itself never pretends a partial neighborhood is prepared.
        next->lightDirty=repair;
    }
    next->configureLight(loadedSeed);state.swap(next);seed=loadedSeed;++revision;
    state->fluidRandom.setSeed(loadedSeed);
    state->entityRandom.setSeed(loadedSeed^0x5deece66);
    for(const auto& [key,record]:state->records){loadFluidTicks(*record);loadEntities(*record);}
    activateFluidChunks();
    return true;
}
}
