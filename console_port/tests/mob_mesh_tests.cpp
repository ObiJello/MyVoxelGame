#include "MobMesh.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static bool near(float a,float b){return std::abs(a-b)<1e-5f;}

int main(){try{
    using namespace console;
    SimulatedEntity entity{L"Zombie",{10.5,80,20.5},{},0};
    const int light=(12<<20)|(5<<4);
    const auto zombie=buildMobMesh(entity,light);
    require(zombie.size()==6*6*6,"Zombie has six original ModelPart cubes");
    bool top=false,feet=false,headFace=false;
    for(const auto& vertex:zombie){
        require(std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z),"Mob position is finite");
        require(vertex.u>=0 && vertex.u<=1 && vertex.v>=0 && vertex.v<=1,"Source skin coordinates fit the 64x32 atlas");
        require(near(vertex.lightU,5.5f/16) && near(vertex.lightV,12.5f/16),"Mob vertices use the world lightmap");
        top|=near(vertex.y,82.0078125f);feet|=near(vertex.y,80.0078125f);
        headFace|=vertex.u>8.f/64 && vertex.u<16.f/64 && vertex.v>8.f/32 && vertex.v<16.f/32;
    }
    require(top && feet,"Humanoid head and feet match the source 24-pixel body height");
    require(headFace,"Head front uses the source skin area");
    entity.yaw=180;
    const auto turned=buildMobMesh(entity,light);
    require(near(zombie[24].z+turned[24].z,41.f),"Source body yaw rotates the front face about the entity");
    // QuadrupedModel::render for a calf: the body at half size (lower), the
    // head at full size.
    SimulatedEntity cow{L"Cow",{10.5,80,20.5},{},0};
    const auto adult=buildMobMesh(cow,light);
    cow.baby=true;
    const auto calf=buildMobMesh(cow,light);
    float adultTop=0,calfTop=0,calfHeadWidth=0,adultHeadWidth=0;
    for(const auto& v:adult)adultTop=std::max(adultTop,v.y);
    for(const auto& v:calf)calfTop=std::max(calfTop,v.y);
    // The head is the first cube (36 vertices): its width along x is kept.
    auto headWidth=[](const std::vector<Vertex>& mesh){float lo=1e9f,hi=-1e9f;for(int i=0;i<36;++i){lo=std::min(lo,mesh[i].x);hi=std::max(hi,mesh[i].x);}return hi-lo;};
    adultHeadWidth=headWidth(adult);calfHeadWidth=headWidth(calf);
    require(calf.size()==adult.size() && calfTop<adultTop && near(calfHeadWidth,adultHeadWidth),
            "A calf is smaller but keeps a full-size head");
    entity.id=L"Skeleton";
    const auto skeleton=buildMobMesh(entity,light);
    require(skeleton.size()==zombie.size(),"Skeleton replaces limb boxes without dropping faces");
    entity.id=L"Spider";
    const auto spider=buildMobMesh(entity,light);
    require(spider.size()==11*6*6,"Spider includes head, body and eight legs");
    entity.id=L"CaveSpider";
    const auto caveSpider=buildMobMesh(entity,light);
    require(caveSpider.size()==spider.size(),"Cave spider reuses the source SpiderModel");
    float spiderReach=0,caveSpiderReach=0;
    for(const auto& vertex:spider)spiderReach=std::max(spiderReach,std::hypot(vertex.x-10.5f,vertex.z-20.5f));
    for(const auto& vertex:caveSpider)caveSpiderReach=std::max(caveSpiderReach,std::hypot(vertex.x-10.5f,vertex.z-20.5f));
    require(near(caveSpiderReach/spiderReach,.7f),"Cave spider uses the source .7 model scale");
    entity.id=L"Silverfish";
    const auto silverfish=buildMobMesh(entity,light);
    require(silverfish.size()==10*6*6,"Silverfish has seven body segments and three shell layers");
    for(const auto& vertex:silverfish)
        require(std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z) &&
                vertex.u>=0 && vertex.u<=1 && vertex.v>=0 && vertex.v<=1,
                "Silverfish source model has finite geometry and atlas UVs");
    entity.id=L"PigZombie";
    require(buildMobMesh(entity,light).size()==zombie.size(),
            "Zombie Pigman reuses the source ZombieModel");
    entity.id=L"Villager";
    const auto villager=buildMobMesh(entity,light);
    require(villager.size()==9*6*6,"VillagerModel includes nose, robe, joined arms and legs");
    bool lowerAtlas=false;
    for(const auto& vertex:villager){
        require(vertex.u>=0 && vertex.u<=1 && vertex.v>=0 && vertex.v<=1,
                "Villager uses its 64x64 source atlas");
        lowerAtlas|=vertex.v>.5f;
    }
    require(lowerAtlas,"Villager robe samples the lower half of its source atlas");
    entity.id=L"Ozelot";
    const auto ozelot=buildMobMesh(entity,light);
    require(ozelot.size()==11*6*6,"OzelotModel has four head cubes, body, tail and four legs");
    for(const auto& vertex:ozelot)
        require(std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z) &&
                vertex.u>=0 && vertex.u<=1 && vertex.v>=0 && vertex.v<=1,
                "Ozelot source model has finite geometry and atlas UVs");
    entity.id=L"Wolf";entity.collarColor=14;
    const auto wolf=buildMobMesh(entity,light),collar=buildMobMesh(entity,light,true);
    require(wolf.size()==11*6*6 && collar.size()==wolf.size(),
            "WolfModel includes head detail, body, tail and four legs with collar overlay");
    require(collar.front().r>collar.front().g && collar.front().g==collar.front().b,
            "Wolf collar uses the source red dye default");
    entity.sitting=true;
    const auto sittingWolf=buildMobMesh(entity,light);
    require(sittingWolf.size()==wolf.size() && !near(sittingWolf[4*36].y,wolf[4*36].y),
            "Wolf sitting pose changes ModelPart positions");
    entity.id=L"Creeper";
    require(buildMobMesh(entity,light).size()==6*6*6,"Creeper includes head, body and four legs");
    entity.id=L"Cow";
    require(buildMobMesh(entity,light).size()==9*6*6,"Cow includes horns, udder and four legs");
    entity.id=L"Pig";
    require(buildMobMesh(entity,light).size()==7*6*6,"Pig includes its distinct snout");
    entity.id=L"Sheep";entity.woolColor=14;
    const auto sheep=buildMobMesh(entity,light),wool=buildMobMesh(entity,light,true);
    require(sheep.size()==6*6*6 && wool.size()==6*6*6,"Sheep has separate base and fleece models");
    require(wool.front().r>wool.front().g && wool.front().g==wool.front().b,"Fleece receives original dye tint");
    entity.id=L"Chicken";
    require(buildMobMesh(entity,light).size()==8*6*6,"Chicken includes beak, wattle and wings");
    constexpr std::array<std::pair<const wchar_t*,int>,6> remainingModels{{
        {L"Slime",4},{L"LavaSlime",9},{L"Ghast",10},
        {L"Blaze",13},{L"Squid",9},{L"Enderman",7}}};
    for(const auto& [id,parts]:remainingModels){
        entity.id=id;
        const auto mesh=buildMobMesh(entity,light);
        require(mesh.size()==size_t(parts*36),"Source creature has its distinct part count");
        for(const auto& vertex:mesh)
            require(std::isfinite(vertex.x) && std::isfinite(vertex.y) &&
                    std::isfinite(vertex.z) && vertex.u>=0 && vertex.u<=1 &&
                    vertex.v>=0 && vertex.v<=1,
                "New creature mesh stays within its source skin atlas");
    }
    entity.id=L"Ghast";
    float ghastLow=1000,ghastHigh=-1000;
    for(const auto& vertex:buildMobMesh(entity,light)){
        ghastLow=std::min(ghastLow,vertex.y);
        ghastHigh=std::max(ghastHigh,vertex.y);
    }
    require(ghastHigh-ghastLow>4.f && ghastLow<80.1f && ghastHigh>84.f,
            "GhastRenderer 4.5 scale and .6 model shift match its four-block size");
    for(const wchar_t* id:{L"Blaze",L"LavaSlime"}){
        entity.id=id;
        const auto fullbright=buildMobMesh(entity,light);
        require(near(fullbright.front().lightU,15.5f/16) &&
                near(fullbright.front().lightV,15.5f/16),
                "Blaze and Magma Cube use source full-bright mob lighting");
    }
    entity.id=L"Enderman";
    const auto eyes=buildMobMesh(entity,light,true);
    require(eyes.size()==7*36,"Enderman eye layer repeats its seven model cubes");
    require(near(eyes.front().lightU,15.5f/16) && near(eyes.front().lightV,15.5f/16),
            "Enderman eye layer is full-bright");
    require(near(eyes.front().r,1),"Enderman eye layer is unshaded");
    entity.id=L"Slime";entity.slimeSize=4;
    require(buildMobMesh(entity,light,true).size()==36,
            "Slime draws its translucent shell after the inner model");
    float maxY=-100;
    for(const auto& vertex:buildMobMesh(entity,light,true))maxY=std::max(maxY,vertex.y);
    require(maxY>81.9f,"Slime source size scales its model from the feet");
    entity.id=L"MushroomCow";
    require(buildMobMesh(entity,light).size()==9*36 &&
            buildMushroomCowMushrooms(entity,light).size()==3*4*6,
            "Mushroom cow uses cow geometry and three red mushroom crosses");
    entity.id=L"Unknown";
    require(buildMobMesh(entity,light).empty(),"Unsupported entities do not borrow a wrong skin");
    std::cout<<"Source mob model boxes, skin UVs and lightmap coordinates passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
