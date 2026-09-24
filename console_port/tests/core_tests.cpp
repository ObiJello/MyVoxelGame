#include "World.h"
#include "Random.h"
#include "Mth.h"
#include "ChunkGenerator.h"
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){
    using namespace console;
    auto temp=std::filesystem::temp_directory_path()/("console-port-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        Random random(0);
        for(int expected:{-1155484576,-723955400,1033096058,-1690734402,-1557280266})check(random.nextInt()==expected,"Random integer vector mismatch");
        random.setSeed(0);check(random.nextLong()==-4962768465676381896LL,"Random long vector mismatch");
        random.setSeed(0);check(std::abs(random.nextDouble()-.730967787376657)<1e-15,"Random double vector mismatch");
        for(int bound:{1,2,31,256,1073741825,2147483647})for(int i=0;i<1000;++i){int n=random.nextInt(bound);check(n>=0 && n<bound,"Bounded random out of range");}
        check(Mth::floor(-.1)==-1 && Mth::lfloor(-16777216.1)==-16777217,"Negative floor mismatch");
        check(Mth::intFloorDiv(-17,16)==-2,"Negative floor division mismatch");
        check(Mth::wrapDegrees(541.0)==-179,"Angle wrap mismatch");
        std::vector<std::future<float>> mathWorkers;
        for(int i=0;i<8;++i)mathWorkers.push_back(std::async(std::launch::async,[]{return Mth::sin(0)+Mth::cos(0);}));
        for(auto& worker:mathWorkers)check(worker.get()==1,"Concurrent sine table initialization failed");
        World a,b;a.generate(42);b.generate(42);check(a.blockSnapshot()==b.blockSnapshot(),"World is not deterministic");
        ChunkGenerator original(42);auto center=original.generate(0,0);
        // World::generate now applies the source feature pass after raw chunk
        // generation, so ore, trees, and plants intentionally differ here.
        int decoratedCells=0;
        for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<World::height;++y){
            const auto raw=y<128?center.blocks[(x*16+z)*128+y]:0;
            if(a.get(x+64,y,z+64)!=raw)++decoratedCells;
        }
        check(decoratedCells<8192,"Natural decoration replaced too much of the original raw chunk");
        b.generate(-42);check(a.blockSnapshot()!=b.blockSnapshot(),"World ignores seed");
        check(!a.collides(a.spawn()),"Spawn intersects terrain");
        check(a.collides({-.1,60,64}),"World boundary is not solid");
        World empty;empty.set(5,5,5,Stone);
        auto hit=empty.raycast({5.5,9,5.5},{0,-1,0},4);
        check(hit.hit && hit.x==5 && hit.y==5 && hit.py==6,"Vertical raycast/placement mismatch");
        check(!empty.raycast({5.5,9,5.5},{0,-1,0},2).hit,"Raycast exceeded reach");
        check(!empty.raycast({5.5,9,5.5},{0,0,0}).hit,"Zero ray should not hit");
        hit=empty.raycast({2,5.5,5.5},{1,0,0});check(hit.hit && hit.px==4,"Horizontal raycast mismatch");
        hit=empty.raycast({8,5.5,5.5},{-1,0,0});check(hit.hit && hit.px==6,"Original hit face sets the adjacent placement cell");
        check(!empty.raycast({5.5,9,5.5},{0,-1,0},-1).hit,"Negative selection reach accepted");
        check(!empty.raycast({std::numeric_limits<double>::infinity(),9,5.5},{0,-1,0}).hit,"Nonfinite selection origin accepted");
        empty.set(4,5,5,Water);hit=empty.raycast({2,5.5,5.5},{1,0,0});check(hit.hit && hit.x==5,"Client selection must pass through its static water");
        check(empty.collides({5.5,5,5.5}) && !empty.collides({5.5,6.001,5.5}),"Player collision mismatch");
        check(!empty.set(-1,1,1,Stone),"Out-of-bounds edit accepted");
        check(!empty.set(1,1,1,static_cast<Block>(255)),"Invalid block accepted");
        std::filesystem::create_directories(temp);auto path=temp/"world.mcp";
        a.set(64,80,64,Bricks);a.save(path);check(b.load(path),"Save missing");check(a.seed==b.seed && a.blockSnapshot()==b.blockSnapshot(),"Save roundtrip mismatch");
        a.set(64,80,64,Glass);a.save(path);check(b.load(path) && b.get(64,80,64)==Glass,"Existing save was not replaced");
        // Existing saves from the first 96-high prototype must survive the move
        // to the full 256-high chunk storage.
        std::vector<std::uint8_t> legacy(World::width*96*World::depth,Air);
        legacy[(23*World::width+17)*96+95]=Bricks;
        std::uint64_t hash=14695981039346656037ULL;
        for(auto value:legacy)hash=(hash^value)*1099511628211ULL;
        auto legacyPath=temp/"legacy.mcp";std::ofstream legacyOut(legacyPath,std::ios::binary);
        auto write64=[&](std::uint64_t value){for(int i=0;i<8;++i)legacyOut.put(static_cast<char>(value>>(8*i)));};
        legacyOut.write("MCPORT01",8);write64(123);write64(legacy.size());write64(hash);
        legacyOut.write(reinterpret_cast<const char*>(legacy.data()),legacy.size());legacyOut.close();
        World migrated;check(migrated.load(legacyPath) && migrated.seed==123,"Legacy save migration failed");
        check(migrated.get(17,95,23)==Bricks && migrated.get(17,96,23)==Air,"Legacy columns were not expanded correctly");
        migrated.save(legacyPath);World migratedAgain;check(migratedAgain.load(legacyPath) && migratedAgain.blockSnapshot()==migrated.blockSnapshot(),"Migrated save roundtrip failed");
        auto preserved=b.blockSnapshot();std::fstream damage(path,std::ios::binary|std::ios::in|std::ios::out);damage.seekp(0);damage.write("BROKEN!!",8);damage.close();
        bool rejected=false;try{b.load(path);}catch(const std::exception&){rejected=true;}
        check(rejected && b.blockSnapshot()==preserved,"Damaged save changed the live world");
        std::ofstream truncated(path,std::ios::binary|std::ios::trunc);truncated<<"MCPORT01";truncated.close();
        rejected=false;try{b.load(path);}catch(const std::exception&){rejected=true;}check(rejected,"Truncated save accepted");
        check(!b.load(temp/"missing.mcp"),"Missing save should return false");
        std::filesystem::remove_all(temp);
        std::cout<<"Passed: random vectors, negative math, deterministic terrain, spawn/collision, raycast, edits, atomic save replacement, corruption rejection\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}
}
