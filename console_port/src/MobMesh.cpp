#include "MobMesh.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace console {
namespace {
constexpr float pi=3.14159265358979323846f;
struct Point { float x,y,z; };
struct Part {
    float x,y,z;
    int width,height,depth,u,v;
    Point pivot;
    float xRot=0,yRot=0,zRot=0;
    bool mirror=false,humanoid=false;
    float grow=0;
    int textureWidth=64,textureHeight=32;
};
Point rotate(Point point,const Part& part){
    const float cx=std::cos(part.xRot),sx=std::sin(part.xRot);
    const float cy=std::cos(part.yRot),sy=std::sin(part.yRot);
    const float cz=std::cos(part.zRot),sz=std::sin(part.zRot);
    // ModelPart applies Z, then Y, then X rotations as GL matrix calls.
    point={point.x,point.y*cx-point.z*sx,point.y*sx+point.z*cx};
    point={point.x*cy+point.z*sy,point.y,-point.x*sy+point.z*cy};
    point={point.x*cz-point.y*sz,point.x*sz+point.y*cz,point.z};
    return {point.x+part.pivot.x,point.y+part.pivot.y,point.z+part.pivot.z};
}
void addPart(std::vector<Vertex>& mesh,const Part& part,const SimulatedEntity& entity,int light,float modelScale){
    // Cube.cpp constructs these eight corners, then its six _Polygon faces.
    float x0=part.x-part.grow,x1=part.x+part.width+part.grow;
    if(part.mirror)std::swap(x0,x1);
    const float y0=part.y-part.grow,y1=part.y+part.height+part.grow;
    const float z0=part.z-part.grow,z1=part.z+part.depth+part.grow;
    const std::array<Point,8> corner{{
        {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
        {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}
    }};
    struct Face { std::array<int,4> point;int u0,v0,u1,v1;float shade; };
    const int u=part.u,v=part.v,w=part.width,h=part.height,d=part.depth;
    const std::array<Face,6> faces{{
        {{{5,1,2,6}},u+d+w,v+d,u+d+w+d,v+d+h,.82f},
        {{{0,4,7,3}},u,v+d,u+d,v+d+h,.82f},
        {{{5,4,0,1}},u+d,v,u+d+w,v+d,1.f},
        {{{2,3,7,6}},u+d+w,part.humanoid?v:v+d,u+d+w+w,part.humanoid?v+d:v,.65f},
        {{{1,0,3,2}},u+d,v+d,u+d+w,v+d+h,.9f},
        {{{4,5,6,7}},u+d+w+d,v+d,u+d+w+d+w,v+d+h,.9f}
    }};
    const float lu=(((light>>4)&15)+.5f)/16.f;
    const float lv=(((light>>20)&15)+.5f)/16.f;
    const float bodyAngle=pi-entity.yaw*pi/180.f;
    const float bodyCos=std::cos(bodyAngle),bodySin=std::sin(bodyAngle);
    for(const auto& face:faces){
        // _Polygon assigns (u1,v0),(u0,v0),(u0,v1),(u1,v1), with
        // the source's 0.1-pixel edge inset and reversed V on normal bottoms.
        const float insetU=(face.u1>face.u0?.1f:-.1f);
        const float insetV=(face.v1>face.v0?.1f:-.1f);
        const std::array<float,4> us{{(face.u1-insetU)/part.textureWidth,(face.u0+insetU)/part.textureWidth,
                                      (face.u0+insetU)/part.textureWidth,(face.u1-insetU)/part.textureWidth}};
        const std::array<float,4> vs{{(face.v0+insetV)/part.textureHeight,(face.v0+insetV)/part.textureHeight,
                                      (face.v1-insetV)/part.textureHeight,(face.v1-insetV)/part.textureHeight}};
        std::array<Vertex,4> quad;
        for(int i=0;i<4;++i){
            const int sourceIndex=face.point[part.mirror?3-i:i];
            const Point p=rotate(corner[sourceIndex],part);
            const float modelX=-p.x/16.f*modelScale,modelZ=p.z/16.f*modelScale;
            // MobRenderer uses glScalef(-1,-1,1) and translates the model
            // origin 24/16 blocks above the entity's feet.
            quad[i]={float(entity.position.x)+modelX*bodyCos+modelZ*bodySin,
                     float(entity.position.y)+(1.5078125f-p.y/16.f)*modelScale+
                         (entity.id==L"Ghast"?-.6f*modelScale:0.f),
                     float(entity.position.z)-modelX*bodySin+modelZ*bodyCos,
                     us[i],vs[i],face.shade,face.shade,face.shade,1,lu,lv};
        }
        for(int index:{0,1,2,0,2,3})mesh.push_back(quad[index]);
    }
}
void quadruped(std::vector<Part>& parts,const SimulatedEntity& entity,bool wool){
    const bool pig=entity.id==L"Pig",cow=entity.id==L"Cow" || entity.id==L"MushroomCow",sheep=entity.id==L"Sheep";
    const int legSize=pig?6:12;
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    if(cow){
        parts.push_back({-4,-4,-6,8,8,6,0,0,{0,4,-8}});
        parts.push_back({-5,-5,-4,1,3,1,22,0,{0,4,-8}});
        parts.push_back({4,-5,-4,1,3,1,22,0,{0,4,-8}});
        parts.push_back({-6,-10,-7,12,18,10,18,4,{0,5,2},pi/2});
        parts.push_back({-2,2,-8,4,6,1,52,0,{0,5,2},pi/2});
    }else if(sheep){
        if(wool){
            parts.push_back({-3,-4,-4,6,6,6,0,0,{0,6,-8},0,0,0,false,false,.6f});
            parts.push_back({-4,-10,-7,8,16,6,28,8,{0,5,2},pi/2,0,0,false,false,1.75f});
        }else{
            parts.push_back({-3,-4,-6,6,6,8,0,0,{0,6,-8}});
            parts.push_back({-4,-10,-7,8,16,6,28,8,{0,5,2},pi/2});
        }
    }else{
        parts.push_back({-4,-4,-8,8,8,8,0,0,{0,float(18-legSize),-6}});
        parts.push_back({-5,-10,-7,10,16,8,28,8,{0,float(17-legSize),2},pi/2});
        parts.push_back({-2,0,-9,4,3,1,16,16,{0,float(18-legSize),-6}});
    }
    for(int i=0;i<4;++i){
        const bool right=i%2,back=i<2;
        const float x=(right?3.f:-3.f)+(cow?(right?1.f:-1.f):0.f);
        const float z=(back?7.f:-5.f)-(cow && !back?1.f:0.f);
        const float angle=std::cos(phase+((i==1 || i==2)?pi:0))*1.4f*stride;
        parts.push_back({-2,0,-2,4,wool?6:legSize,4,0,16,
                         {x,float(24-legSize),z},angle,0,0,false,false,wool?.5f:0.f});
    }
}
void chicken(std::vector<Part>& parts,const SimulatedEntity& entity){
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    parts.push_back({-2,-6,-2,4,6,3,0,0,{0,15,-4}});
    parts.push_back({-2,-4,-4,4,2,2,14,0,{0,15,-4}});
    parts.push_back({-1,-2,-3,2,2,2,14,4,{0,15,-4}});
    parts.push_back({-3,-4,-3,6,8,6,0,9,{0,16,0},pi/2});
    for(int side=0;side<2;++side){
        const float legAngle=std::cos(phase+(side?pi:0))*1.4f*stride;
        parts.push_back({-1,0,-3,3,5,3,26,0,{side?1.f:-2.f,19,1},legAngle});
        const float flap=std::sin(entity.age*.5f)*.4f;
        parts.push_back({side?-1.f:0.f,0,-3,1,4,6,24,13,
                         {side?4.f:-4.f,13,0},0,0,side?-flap:flap});
    }
}
void creeper(std::vector<Part>& parts,const SimulatedEntity& entity){
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    parts.push_back({-4,-8,-4,8,8,8,0,0,{0,4,0}});
    parts.push_back({-4,0,-2,8,12,4,16,16,{0,4,0}});
    for(int i=0;i<4;++i){
        const bool right=i%2,back=i<2;
        const float angle=std::cos(phase+((i==1 || i==2)?pi:0))*1.4f*stride;
        parts.push_back({-2,0,-2,4,6,4,0,16,
                         {right?2.f:-2.f,16,back?4.f:-4.f},angle});
    }
}
void humanoid(std::vector<Part>& parts,bool skeleton,bool zombie,const SimulatedEntity& entity){
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    parts.push_back({-4,-8,-4,8,8,8,0,0,{0,0,0},0,0,0,false,true});
    parts.push_back({-4,0,-2,8,12,4,16,16,{0,0,0},0,0,0,false,true});
    for(int side=0;side<2;++side){
        const bool right=side==1;
        const float armX=right?5.f:-5.f;
        const float angle=(zombie?-pi/2:0)+
            (zombie?0:std::cos(phase+(right?0:pi))*stride);
        if(skeleton)parts.push_back({-1,-2,-1,2,12,2,40,16,{armX,2,0},angle,0,0,right,false});
        else parts.push_back({right?-1.f:-3.f,-2,-2,4,12,4,40,16,{armX,2,0},angle,0,0,right,true});
    }
    for(int side=0;side<2;++side){
        const bool right=side==1;
        const float angle=std::cos(phase+(right?pi:0))*1.4f*stride;
        if(skeleton)parts.push_back({-1,0,-1,2,12,2,0,16,{right?2.f:-2.f,12,0},angle,0,0,right,false});
        else parts.push_back({-2,0,-2,4,12,4,0,16,{right?1.9f:-1.9f,12,0},angle,0,0,right,true});
    }
}
void spider(std::vector<Part>& parts,const SimulatedEntity& entity){
    parts.push_back({-4,-4,-8,8,8,8,32,4,{0,15,-3}});
    parts.push_back({-3,-3,-3,6,6,6,0,0,{0,15,0}});
    parts.push_back({-5,-4,-6,10,8,12,0,12,{0,15,9}});
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    for(int i=0;i<8;++i){
        const bool right=i%2;
        const int group=i/2;
        const float wave=phase*2+pi*.5f*std::array<int,4>{0,2,1,3}[group];
        const float yStep=-std::cos(wave)*.4f*stride;
        const float zStep=std::abs(std::sin(phase+pi*.5f*std::array<int,4>{0,2,1,3}[group]))*.4f*stride;
        const float zAngle=(i<2 || i>=6?pi/4:pi/4*.74f)*(right?1:-1)+zStep*(right?-1:1);
        const float yAngle=(i<2?pi/4:i<4?pi/8:i<6?-pi/8:-pi/4)*(right?-1:1)+yStep*(right?-1:1);
        parts.push_back({right?-1.f:-15.f,-1,-1,16,2,2,18,0,
                         {right?4.f:-4.f,15,float(2-i/2)},0,yAngle,zAngle});
    }
}
void silverfish(std::vector<Part>& parts,const SimulatedEntity& entity){
    // SilverfishModel::BODY_SIZES/BODY_TEXS and bodyLayers, including the
    // source's staggered yaw and sideways travel animation.
    constexpr std::array<std::array<int,3>,7> sizes{{
        {{3,2,2}},{{4,3,2}},{{6,4,3}},{{3,3,3}},{{2,2,3}},{{2,1,2}},{{1,1,2}}
    }};
    constexpr std::array<std::array<int,2>,7> tex{{
        {{0,0}},{{0,4}},{{0,9}},{{0,16}},{{0,22}},{{11,0}},{{13,4}}
    }};
    std::array<float,7> z{},x{},yaw{};
    float placement=-3.5f;
    const float bob=entity.age*.6662f;
    for(int i=0;i<7;++i){
        z[i]=placement;
        yaw[i]=std::cos(bob*.9f+i*.15f*pi)*pi*.05f*(1+std::abs(i-2));
        x[i]=std::sin(bob*.9f+i*.15f*pi)*pi*.2f*std::abs(i-2);
        parts.push_back({-sizes[i][0]*.5f,0,-sizes[i][2]*.5f,
                         sizes[i][0],sizes[i][1],sizes[i][2],tex[i][0],tex[i][1],
                         {x[i],float(24-sizes[i][1]),z[i]},0,yaw[i]});
        if(i<6)placement+=(sizes[i][2]+sizes[i+1][2])*.5f;
    }
    parts.push_back({-5,0,-sizes[2][2]*.5f,10,8,sizes[2][2],20,0,
                     {0,16,z[2]},0,yaw[2]});
    parts.push_back({-3,0,-sizes[4][2]*.5f,6,4,sizes[4][2],20,11,
                     {x[4],20,z[4]},0,yaw[4]});
    parts.push_back({-3,0,-sizes[4][2]*.5f,6,5,sizes[1][2],20,18,
                     {x[1],19,z[1]},0,yaw[1]});
}
void villager(std::vector<Part>& parts,const SimulatedEntity& entity){
    // VillagerModel::_init: two head cubes, two body cubes, joined arms, legs.
    // Its skin is 64x64, unlike the 64x32 skins above.
    const auto add=[&](Part part){part.textureHeight=64;parts.push_back(part);};
    add({-4,-10,-4,8,10,8,0,0,{0,0,0}});
    add({-1,-3,-6,2,4,2,24,0,{0,0,0}});
    add({-4,0,-3,8,12,6,16,20,{0,0,0}});
    add({-4,0,-3,8,18,6,0,38,{0,0,0},0,0,0,false,false,.5f});
    add({-8,-2,-2,4,8,4,44,22,{0,3,-1},-.75f});
    add({4,-2,-2,4,8,4,44,22,{0,3,-1},-.75f});
    add({-4,2,-2,8,4,4,40,38,{0,3,-1},-.75f});
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    for(int side=0;side<2;++side){
        const float angle=std::cos(phase+(side?pi:0))*1.4f*stride*.5f;
        add({-2,0,-2,4,12,4,0,22,{side?2.f:-2.f,12,0},angle,0,0,side==1});
    }
}
void ozelot(std::vector<Part>& parts,const SimulatedEntity& entity){
    // OzelotModel WALK_STATE geometry and four-way gait.
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    constexpr Point head{0,15,-9};
    parts.push_back({-2.5f,-2,-3,5,4,5,0,0,head});
    parts.push_back({-1.5f,0,-4,3,2,2,0,24,head});
    parts.push_back({-2,-3,0,1,1,2,0,10,head});
    parts.push_back({1,-3,0,1,1,2,6,10,head});
    parts.push_back({-2,3,-8,4,16,6,20,0,{0,12,-10},pi/2});
    parts.push_back({-.5f,0,0,1,8,1,0,15,{0,15,8},.9f});
    parts.push_back({-.5f,0,0,1,8,1,4,15,{0,20,14},.55f*pi+.25f*pi*std::cos(float(entity.age))*stride});
    const float backLeft=std::cos(phase)*stride,backRight=std::cos(phase+pi)*stride;
    parts.push_back({-1,0,1,2,6,2,8,13,{1.1f,18,5},backLeft});
    parts.push_back({-1,0,1,2,6,2,8,13,{-1.1f,18,5},backRight});
    parts.push_back({-1,0,0,2,10,2,40,0,{1.2f,13.8f,-5},backRight});
    parts.push_back({-1,0,0,2,10,2,40,0,{-1.2f,13.8f,-5},backLeft});
}
void wolf(std::vector<Part>& parts,const SimulatedEntity& entity){
    // WolfModel standing and sitting poses, including the shared head cubes.
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    constexpr Point head{-1,13.5f,-7};
    parts.push_back({-3,-3,-2,6,6,4,0,0,head});
    parts.push_back({-3,-5,0,2,2,1,16,14,head});
    parts.push_back({1,-5,0,2,2,1,16,14,head});
    parts.push_back({-1.5f,0,-5,3,3,4,0,10,head});
    const bool sit=entity.sitting;
    parts.push_back({-4,-2,-3,6,9,6,18,14,sit?Point{0,18,0}:Point{0,14,2},
                     sit?.25f*pi:pi/2});
    parts.push_back({-4,-3,-3,8,6,7,21,0,
                     sit?Point{-1,16,-3}:Point{-1,14,-3},sit?.4f*pi:pi/2});
    parts.push_back({-1,0,-1,2,8,2,9,18,sit?Point{-1,21,6}:Point{-1,12,8},
                     sit?.55f*pi:float(std::cos(phase)*stride*.2f)});
    for(int i=0;i<4;++i){
        const bool right=i%2,back=i<2;
        const float x=right?.5f:-2.5f;
        const float z=back?7.f:-4.f;
        const float angle=sit?(back?1.5f*pi:1.85f*pi):
            std::cos(phase+((i==1 || i==2)?pi:0))*1.4f*stride;
        parts.push_back({-1,0,-1,2,8,2,0,18,
                         sit?Point{x,back?22.f:17.f,back?2.f:-4.f}:Point{x,16,z},angle});
    }
}
void slime(std::vector<Part>& parts,bool shell){
    // SlimeModel(0) draws the transparent shell; SlimeModel(16) draws the core,
    // eyes, and mouth in the first render pass.
    if(shell){parts.push_back({-4,16,-4,8,8,8,0,0,{0,0,0}});return;}
    parts.push_back({-3,17,-3,6,6,6,0,16,{0,0,0}});
    parts.push_back({-3.25f,18,-3.5f,2,2,2,32,0,{0,0,0}});
    parts.push_back({1.25f,18,-3.5f,2,2,2,32,4,{0,0,0}});
    parts.push_back({0,21,-3.5f,1,1,1,32,8,{0,0,0}});
}
void lavaSlime(std::vector<Part>& parts,const SimulatedEntity& entity){
    parts.push_back({-2,18,-2,4,4,4,0,16,{0,0,0}});
    for(int i=0;i<8;++i){
        const int u=i==2 || i==3?24:0;
        const int v=i==2?10:i==3?19:i;
        const float squish=std::max(0.f,std::sin(entity.age*.35f))*.14f;
        parts.push_back({-4,float(16+i),-4,8,1,8,u,v,{0,float(-(4-i)*squish*1.7f),0}});
    }
}
void ghast(std::vector<Part>& parts,const SimulatedEntity& entity){
    parts.push_back({-8,-8,-8,16,16,16,0,0,{0,8,0}});
    // GhastModel seeds its nine tentacle lengths once; preserve stable lengths.
    constexpr std::array<int,9> lengths{{8,13,9,11,11,10,12,9,12}};
    for(int i=0;i<9;++i){
        const float x=((i%3-(i/3%2)*.5f+.25f)-1.f)*5.f;
        const float z=((i/3)-1.f)*5.f;
        parts.push_back({-1,0,-1,2,lengths[i],2,0,0,{x,15,z},
                         .2f*std::sin(entity.age*.3f+i)+.4f});
    }
}
void blaze(std::vector<Part>& parts,const SimulatedEntity& entity){
    const float bob=entity.age*.25f;
    parts.push_back({-4,-4,-4,8,8,8,0,0,{0,0,0}});
    float angle=-bob*pi*.1f;
    for(int i=0;i<12;++i){
        if(i==4)angle=.25f*pi+bob*pi*.03f;
        if(i==8)angle=.15f*pi-bob*pi*.05f;
        const float radius=i<4?9.f:i<8?7.f:5.f;
        const float y=(i<4?-2.f:i<8?2.f:11.f)+
            std::cos((i*(i<8?2.f:1.5f)+bob)*(i<8?.25f:.5f));
        parts.push_back({0,0,0,2,8,2,0,16,{std::cos(angle)*radius,y,std::sin(angle)*radius}});
        angle+=pi*.5f;
    }
}
void squid(std::vector<Part>& parts,const SimulatedEntity& entity){
    parts.push_back({-6,-8,-6,12,16,12,0,0,{0,8,0}});
    for(int i=0;i<8;++i){
        const float angle=i*pi/4.f;
        const float x=std::cos(angle)*5,z=std::sin(angle)*5;
        parts.push_back({-1,0,-1,2,18,2,48,0,{x,15,z},
                         entity.squidTentacleAngle,-angle+pi*.5f});
    }
}
void enderman(std::vector<Part>& parts,const SimulatedEntity& entity){
    const float stride=std::min(1.f,float(std::hypot(entity.velocity.x,entity.velocity.z)*8));
    const float phase=entity.age*.6662f;
    // EndermanModel offsets the humanoid parts by -14 model pixels.
    parts.push_back({-4,-8,-4,8,8,8,0,0,{0,-13,0}});
    parts.push_back({-4,-8,-4,8,8,8,0,16,{0,-13,0},0,0,0,false,false,-.5f});
    parts.push_back({-4,0,-2,8,12,4,32,16,{0,-14,0}});
    for(int side=0;side<2;++side){
        const bool right=side==1;
        const float swing=std::clamp(std::cos(phase+(right?0:pi))*stride,-.4f,.4f);
        parts.push_back({-1,-2,-1,2,30,2,56,0,{right?5.f:-3.f,-12,0},swing,0,0,right});
        parts.push_back({-1,0,-1,2,30,2,56,0,{right?2.f:-2.f,-5,0},-swing,0,0,right});
    }
}
}
std::vector<Vertex> buildMobMesh(const SimulatedEntity& entity,int packedLight,bool coatOverlay){
    std::vector<Part> parts;
    if(coatOverlay && entity.id!=L"Sheep" && entity.id!=L"Wolf" &&
       entity.id!=L"Slime" && entity.id!=L"Enderman")return {};
    if(entity.id==L"Zombie" || entity.id==L"PigZombie")humanoid(parts,false,true,entity);
    else if(entity.id==L"Skeleton")humanoid(parts,true,false,entity);
    else if(entity.id==L"Spider" || entity.id==L"CaveSpider")spider(parts,entity);
    else if(entity.id==L"Silverfish")silverfish(parts,entity);
    else if(entity.id==L"Villager")villager(parts,entity);
    else if(entity.id==L"Ozelot")ozelot(parts,entity);
    else if(entity.id==L"Wolf")wolf(parts,entity);
    else if(entity.id==L"Slime")slime(parts,coatOverlay);
    else if(entity.id==L"LavaSlime" && !coatOverlay)lavaSlime(parts,entity);
    else if(entity.id==L"Ghast")ghast(parts,entity);
    else if(entity.id==L"Blaze")blaze(parts,entity);
    else if(entity.id==L"Squid")squid(parts,entity);
    else if(entity.id==L"Enderman")enderman(parts,entity);
    else if(entity.id==L"Creeper")creeper(parts,entity);
    else if(entity.id==L"Cow" || entity.id==L"MushroomCow" || entity.id==L"Pig" || entity.id==L"Sheep")quadruped(parts,entity,coatOverlay);
    else if(entity.id==L"Chicken")chicken(parts,entity);
    std::vector<Vertex> mesh;mesh.reserve(parts.size()*36);
    // GhastRenderer::scale is 4.5 on an uncharged Ghast; its model is a
    // 16-pixel body inside a four-block collision box.
    const float modelScale=entity.id==L"Ghast"?4.5f:entity.id==L"CaveSpider"?.7f:
        (entity.id==L"Slime" || entity.id==L"LavaSlime")?float(std::clamp(entity.slimeSize,1,4)):1.f;
    const int effectiveLight=entity.id==L"Blaze" || entity.id==L"LavaSlime" ||
        (entity.id==L"Enderman" && coatOverlay)?((15<<20)|(15<<4)):packedLight;
    for(const auto& part:parts)addPart(mesh,part,entity,effectiveLight,modelScale);
    if(entity.id==L"Enderman" && coatOverlay)
        for(auto& vertex:mesh)vertex.r=vertex.g=vertex.b=1;
    if(coatOverlay && (entity.id==L"Sheep" || entity.id==L"Wolf")){
        static constexpr std::array<std::array<float,3>,16> color{{
            {{1,1,1}},{{.85f,.5f,.2f}},{{.7f,.3f,.85f}},{{.4f,.6f,.85f}},
            {{.9f,.9f,.2f}},{{.5f,.8f,.1f}},{{.95f,.5f,.65f}},{{.3f,.3f,.3f}},
            {{.6f,.6f,.6f}},{{.3f,.5f,.65f}},{{.5f,.25f,.7f}},{{.2f,.3f,.7f}},
            {{.4f,.3f,.2f}},{{.4f,.5f,.2f}},{{.6f,.2f,.2f}},{{.1f,.1f,.1f}}
        }};
        const auto& tint=color[std::clamp(entity.id==L"Wolf"?entity.collarColor:entity.woolColor,0,15)];
        for(auto& vertex:mesh){vertex.r*=tint[0];vertex.g*=tint[1];vertex.b*=tint[2];}
    }
    return mesh;
}
std::vector<Vertex> buildMushroomCowMushrooms(const SimulatedEntity& entity,int packedLight){
    if(entity.id!=L"MushroomCow")return {};
    std::vector<Vertex> mesh;mesh.reserve(3*4*6);
    // MushroomCowRenderer places two Tile::mushroom2 crosses on the back and
    // one on the head. PreStitchedTextureMap maps mushroom_red to atlas (12,1).
    constexpr std::array<Point,3> positions{{{-.24f,1.28f,.27f},
                                              {.25f,1.28f,-.21f},
                                              {0,1.62f,-.68f}}};
    const float angle=pi-entity.yaw*pi/180.f,ca=std::cos(angle),sa=std::sin(angle);
    const float u0=(12.f+.02f)/16.f,u1=(13.f-.02f)/16.f;
    const float v0=(1.f+.02f)/16.f,v1=(2.f-.02f)/16.f;
    const float lu=(((packedLight>>4)&15)+.5f)/16.f;
    const float lv=(((packedLight>>20)&15)+.5f)/16.f;
    for(const auto& center:positions){
        for(int cross=0;cross<2;++cross){
            std::array<Vertex,4> quad;
            for(int corner=0;corner<4;++corner){
                const float side=(corner==0 || corner==3)?-.28f:.28f;
                const float localX=center.x+(cross==0?side:0);
                const float localZ=center.z+(cross==1?side:0);
                const float localY=center.y+((corner>=2)?.55f:0);
                quad[corner]={float(entity.position.x)+localX*ca+localZ*sa,
                              float(entity.position.y)+localY,
                              float(entity.position.z)-localX*sa+localZ*ca,
                              corner==0 || corner==3?u0:u1,corner>=2?v0:v1,
                              1,1,1,1,lu,lv};
            }
            for(int index:{0,1,2,0,2,3,2,1,0,3,2,0})mesh.push_back(quad[index]);
        }
    }
    return mesh;
}
}
