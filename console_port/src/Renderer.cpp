#include "Renderer.h"
#include "RendererShaders.h"
#include "CelestialMesh.h"
#include "SkyColour.h"
#include "MobMesh.h"
#include "ExperienceOrbMesh.h"
#include "DroppedItemMesh.h"
#include "HangingMesh.h"
#include <bit>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>

namespace console {
static GLuint shader(GLenum kind,const char* code) {
    GLuint s=glCreateShader(kind);glShaderSource(s,1,&code,nullptr);glCompileShader(s);
    int ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char msg[4096];glGetShaderInfoLog(s,sizeof(msg),nullptr,msg);glDeleteShader(s);throw std::runtime_error(msg);}return s;
}
Renderer::Renderer(const std::filesystem::path& assets) {
    GLuint v=shader(GL_VERTEX_SHADER,terrainVertexShader),f=shader(GL_FRAGMENT_SHADER,terrainFragmentShader);
    program_=glCreateProgram();glAttachShader(program_,v);glAttachShader(program_,f);
    glBindAttribLocation(program_,0,"position");glBindAttribLocation(program_,1,"texcoord");glBindAttribLocation(program_,2,"color");
    glBindAttribLocation(program_,3,"lightcoord");
    glLinkProgram(program_);glDeleteShader(v);glDeleteShader(f);
    int ok;glGetProgramiv(program_,GL_LINK_STATUS,&ok);if(!ok)throw std::runtime_error("Could not link desktop shaders");
    glGenVertexArrays(1,&vao_);glGenBuffers(1,&vbo_);glGenBuffers(1,&terrainVbo_);glGenBuffers(1,&waterVbo_);
    glBindVertexArray(vao_);glBindBuffer(GL_ARRAY_BUFFER,vbo_);
    for(int i=0;i<4;++i)glEnableVertexAttribArray(i);
    glGenTextures(1,&white_);glBindTexture(GL_TEXTURE_2D,white_);unsigned char white[]={255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    for(auto name:{"terrain","items","xporb","art_kz","book","chest","largechest","enderchest","font","icons","gui","logo","panorama_n","panorama_s","button","button_focus","button_down","sun","moon_phases","clouds","mob_zombie","mob_skeleton","mob_skeleton_wither","mob_char","mob_spider","mob_cavespider","mob_silverfish","mob_pigzombie","mob_villager","mob_villager_farmer","mob_villager_librarian","mob_villager_priest","mob_villager_smith","mob_villager_butcher","mob_ozelot","mob_cat_black","mob_cat_red","mob_cat_siamese","mob_wolf","mob_wolf_angry","mob_wolf_tame","mob_wolf_collar","mob_creeper","mob_cow","mob_redcow","mob_pig","mob_sheep","mob_sheep_fur","mob_chicken","mob_slime","mob_lava","mob_ghast","mob_blaze","mob_squid","mob_enderman","mob_enderman_eyes","panel_tl","panel_tm","panel_tr","panel_ml","panel_mm","panel_mr","panel_bl","panel_bm","panel_br","icon_holder","brewing_stand","brewing_arrow_on","brewing_arrow_off","brewing_bubbles_on","brewing_bubbles_off","flame_on","flame_off","arrow_on","arrow_off"})
        textures_[name]=load(assets/(std::string(name)+".png"),std::string(name).starts_with("panorama"));
    int w,h,n;unsigned char* font=stbi_load((assets/"font.png").string().c_str(),&w,&h,&n,4);
    if(!font)throw std::runtime_error("Cannot read font metrics");
    for(int c=0;c<256;++c){int right=0;for(int y=0;y<8;++y)for(int x=0;x<8;++x)
        if(font[(((c/16)*8+y)*w+(c%16)*8+x)*4+3]>127)right=std::max(right,x+1);
        glyphWidths_[c]=right+1;}
    glyphWidths_[32]=4;stbi_image_free(font);
    glGenTextures(1,&lightmap_);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,lightmap_);
    const auto light=buildConsoleLightmap({});
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,light.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(program_);glUniform1i(glGetUniformLocation(program_,"atlas"),0);
    glUniform1i(glGetUniformLocation(program_,"lightmap"),1);
    for(auto spec:liquidAnimationSpecs()) {
        int width,height,channels;
        auto path=assets/"animations"/(std::string(spec.name)+".png");
        unsigned char* pixels=stbi_load(path.string().c_str(),&width,&height,&channels,4);
        if(!pixels)throw std::runtime_error("Cannot read liquid animation: "+path.string());
        if(width!=spec.size || height!=spec.size*spec.frames ||
           textures_.at("terrain").width!=256 || textures_.at("terrain").height!=256) {
            stbi_image_free(pixels);
            throw std::runtime_error("Invalid liquid strip or terrain atlas dimensions");
        }
        std::vector<unsigned char> frames(pixels,pixels+width*height*4);
        stbi_image_free(pixels);
        liquidAnimations_.push_back({spec,TextureAnimation(spec.frames,spec.schedule),std::move(frames)});
        uploadAnimation(liquidAnimations_.size()-1,0);
    }
}
Renderer::~Renderer() {
    if(pendingMesh_)releaseChunkBuffers(pendingMesh_->chunks);
    releaseChunkBuffers(frontChunks_);
    for(auto& [name,t]:textures_)glDeleteTextures(1,&t.id);
    glDeleteTextures(1,&white_);glDeleteBuffers(1,&vbo_);glDeleteBuffers(1,&terrainVbo_);glDeleteBuffers(1,&waterVbo_);
    glDeleteTextures(1,&lightmap_);
    glDeleteVertexArrays(1,&vao_);glDeleteProgram(program_);
}
Texture Renderer::load(const std::filesystem::path& path,bool smooth) {
    Texture t;int n;auto pixels=stbi_load(path.string().c_str(),&t.width,&t.height,&n,4);
    if(!pixels)throw std::runtime_error("Cannot load texture: "+path.string());
    glGenTextures(1,&t.id);glBindTexture(GL_TEXTURE_2D,t.id);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,t.width,t.height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);stbi_image_free(pixels);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,smooth?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,smooth?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    return t;
}
void Renderer::resize(int w,int h){width_=std::max(w,1);height_=std::max(h,1);glViewport(0,0,width_,height_);}
static void attributes() {
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsetof(Vertex,x)));
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsetof(Vertex,u)));
    glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsetof(Vertex,r)));
    glVertexAttribPointer(3,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsetof(Vertex,lightU)));
}
void Renderer::draw(const std::vector<Vertex>& vertices,GLuint texture,GLenum primitive) {
    glBindVertexArray(vao_);glBindBuffer(GL_ARRAY_BUFFER,vbo_);
    glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(Vertex),vertices.data(),GL_STREAM_DRAW);attributes();
    glBindTexture(GL_TEXTURE_2D,texture);glDrawArrays(primitive,0,static_cast<GLsizei>(vertices.size()));
}
void Renderer::rebuild(const World& world) {
    uploadMesh(buildTerrainMesh(world));
}
void Renderer::releaseChunkBuffers(std::array<ChunkBuffers,64>& chunks) {
    for(auto& chunk:chunks){
        if(chunk.opaque)glDeleteBuffers(1,&chunk.opaque);
        if(chunk.water)glDeleteBuffers(1,&chunk.water);
        chunk={};
    }
}
std::size_t Renderer::triangleCount()const {
    std::size_t vertices=chests_.size()+chestLids_.size()+largeChests_.size()+largeChestLids_.size()+enderChests_.size()+enderChestLids_.size();
    for(const auto& skull:skulls_)vertices+=skull.size();
    if(frontSegmented_){
        for(const auto& chunk:frontChunks_)vertices+=chunk.opaqueVertices+chunk.waterVertices;
    }else vertices+=opaque_.size()+water_.size();
    return vertices/3;
}
void Renderer::beginRebuild(const World& world) {
    if(pendingMesh_)releaseChunkBuffers(pendingMesh_->chunks);
    PendingMesh next;
    next.blocks=world.blockSnapshot();next.revision=world.revision;
    next.originX=world.originX();next.originZ=world.originZ();
    pendingMesh_=std::move(next);
}
bool Renderer::stepRebuild(const World& world,int chunkBudget) {
    if(chunkBudget<1)throw std::invalid_argument("Terrain mesh chunk budget must be positive");
    if(!pendingMesh_)return true;
    if(pendingMesh_->originX!=world.originX() || pendingMesh_->originZ!=world.originZ())beginRebuild(world);
    else if(pendingMesh_->revision!=world.revision)pendingMesh_->needsRestart=true;
    auto append=[](auto& target,auto& source){target.insert(target.end(),std::make_move_iterator(source.begin()),std::make_move_iterator(source.end()));};
    for(int i=0;i<chunkBudget && pendingMesh_->nextChunk<64;++i){
        auto& pending=*pendingMesh_;
        const int index=pending.nextChunk,x=pending.originX+(index%8)*16,z=pending.originZ+(index/8)*16;
        auto section=buildTerrainMeshRegion(world,pending.blocks,x,z,16,16);
        append(pending.mesh.chests,section.chests);append(pending.mesh.chestLids,section.chestLids);
        append(pending.mesh.largeChests,section.largeChests);append(pending.mesh.largeChestLids,section.largeChestLids);
        append(pending.mesh.enderChests,section.enderChests);append(pending.mesh.enderChestLids,section.enderChestLids);
        for(int type=0;type<5;++type)append(pending.mesh.skulls[type],section.skulls[type]);
        append(pending.mesh.enchantTables,section.enchantTables);
        auto& gpu=pending.chunks[index];
        if(!section.opaque.empty()){
            glGenBuffers(1,&gpu.opaque);glBindBuffer(GL_ARRAY_BUFFER,gpu.opaque);
            glBufferData(GL_ARRAY_BUFFER,section.opaque.size()*sizeof(Vertex),section.opaque.data(),GL_STATIC_DRAW);
            gpu.opaqueVertices=static_cast<GLsizei>(section.opaque.size());
        }
        if(!section.water.empty()){
            glGenBuffers(1,&gpu.water);glBindBuffer(GL_ARRAY_BUFFER,gpu.water);
            glBufferData(GL_ARRAY_BUFFER,section.water.size()*sizeof(Vertex),section.water.data(),GL_STATIC_DRAW);
            gpu.waterVertices=static_cast<GLsizei>(section.water.size());
        }
        ++pending.nextChunk;
    }
    if(pendingMesh_->nextChunk<64)return false;
    // A block edit may arrive while sections are being built. Keep the last
    // complete mesh visible until the new snapshot is ready; uploading the
    // obsolete one here would flash stale geometry and do an avoidable large
    // GPU transfer on the gameplay thread.
    const bool stale=pendingMesh_->needsRestart || pendingMesh_->revision!=world.revision;
    if(stale && pendingMesh_->restartCount<2){
        const int restartCount=pendingMesh_->restartCount+1;
        beginRebuild(world);
        pendingMesh_->restartCount=restartCount;
        return false;
    }
    releaseChunkBuffers(frontChunks_);
    frontChunks_=pendingMesh_->chunks;
    pendingMesh_->chunks={};
    frontSegmented_=true;
    chests_=std::move(pendingMesh_->mesh.chests);chestLids_=std::move(pendingMesh_->mesh.chestLids);
    largeChests_=std::move(pendingMesh_->mesh.largeChests);largeChestLids_=std::move(pendingMesh_->mesh.largeChestLids);
    enderChests_=std::move(pendingMesh_->mesh.enderChests);enderChestLids_=std::move(pendingMesh_->mesh.enderChestLids);
    skulls_=std::move(pendingMesh_->mesh.skulls);
    enchantTables_=std::move(pendingMesh_->mesh.enchantTables);
    std::vector<Vertex>().swap(opaque_);std::vector<Vertex>().swap(water_);
    glBindBuffer(GL_ARRAY_BUFFER,terrainVbo_);glBufferData(GL_ARRAY_BUFFER,0,nullptr,GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER,waterVbo_);glBufferData(GL_ARRAY_BUFFER,0,nullptr,GL_STATIC_DRAW);
    pendingMesh_.reset();
    // Continuous fluid changes may outpace a 64-section rebuild. Publish a
    // bounded snapshot, then immediately start a newer one instead of showing
    // frozen water forever while restarting an unfinishable pass.
    if(stale){beginRebuild(world);return false;}
    return true;
}
void Renderer::uploadMesh(TerrainMesh mesh) {
    if(pendingMesh_)releaseChunkBuffers(pendingMesh_->chunks);
    pendingMesh_.reset();
    releaseChunkBuffers(frontChunks_);frontSegmented_=false;
    opaque_=std::move(mesh.opaque);water_=std::move(mesh.water);chests_=std::move(mesh.chests);chestLids_=std::move(mesh.chestLids);largeChests_=std::move(mesh.largeChests);largeChestLids_=std::move(mesh.largeChestLids);
    enderChests_=std::move(mesh.enderChests);enderChestLids_=std::move(mesh.enderChestLids);skulls_=std::move(mesh.skulls);
    enchantTables_=std::move(mesh.enchantTables);
    glBindBuffer(GL_ARRAY_BUFFER,terrainVbo_);glBufferData(GL_ARRAY_BUFFER,opaque_.size()*sizeof(Vertex),opaque_.data(),GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER,waterVbo_);glBufferData(GL_ARRAY_BUFFER,water_.size()*sizeof(Vertex),water_.data(),GL_STATIC_DRAW);
}
void Renderer::tickLighting(double seconds) {
    if(!std::isfinite(seconds) || seconds<0 || seconds>1)throw std::invalid_argument("Invalid lighting frame interval");
    lightTickSeconds_+=seconds;
    while(lightTickSeconds_>=.05){
        cloudTicks_=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(cloudTicks_)+1);
        std::array<double,8> randomValues;for(auto& value:randomValues)value=lightRandom_.nextDouble();
        flicker_.tick(randomValues);lightTickSeconds_-=.05;
        for(std::size_t i=0;i<liquidAnimations_.size();++i) {
            int frame=liquidAnimations_[i].animation.tick();
            if(frame>=0)uploadAnimation(i,frame);
        }
    }
}
void Renderer::uploadAnimation(std::size_t index,int frame) {
    const auto& animation=liquidAnimations_.at(index);
    const auto& spec=animation.spec;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,textures_.at("terrain").id);
    glTexSubImage2D(GL_TEXTURE_2D,0,spec.x,spec.y,spec.size,spec.size,GL_RGBA,
        GL_UNSIGNED_BYTE,animation.pixels.data()+frame*spec.size*spec.size*4);
}
void Renderer::world(const World& source,Vec3 eye,double yaw,double pitch,double distance,const Hit& hit) {
    glUseProgram(program_);glEnable(GL_DEPTH_TEST);glEnable(GL_CULL_FACE);glEnable(GL_BLEND);
    LightmapInput input;input.skyDarken=source.skyDarken();input.flicker=flicker_.red();
    const int nightVision=source.potionEffectDuration(16);
    input.nightVisionTicks=nightVision>0?nightVision:-1;
    const auto light=buildConsoleLightmap(input);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,lightmap_);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,light.data());
    glActiveTexture(GL_TEXTURE0);glUniform1i(glGetUniformLocation(program_,"useLighting"),1);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_TRUE);
    auto skyColour=source.skyColour(std::clamp(int(std::floor(eye.x)),source.originX(),source.originX()+World::width-1),std::clamp(int(std::floor(eye.z)),source.originZ(),source.originZ()+World::depth-1));
    glClearColor(skyColour[0],skyColour[1],skyColour[2],1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glm::vec3 e(eye.x,eye.y,eye.z),dir(std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch));
    glm::mat4 matrix=glm::perspective(glm::radians(70.f),float(width_)/height_,.05f,400.f)*glm::lookAt(e,e+dir,{0,1,0});
    glUniformMatrix4fv(glGetUniformLocation(program_,"matrix"),1,GL_FALSE,glm::value_ptr(matrix));
    glUniform3fv(glGetUniformLocation(program_,"eye"),1,glm::value_ptr(e));
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDepthMask(GL_FALSE);
    glUniform1i(glGetUniformLocation(program_,"useLighting"),0);
    glUniform1f(glGetUniformLocation(program_,"fogDistance"),0);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE);
    glUniform1i(glGetUniformLocation(program_,"skyPass"),1);
    auto celestial=buildCelestialMesh(source.time(),source.rainLevel());
    for(auto* mesh:{&celestial.sun,&celestial.moon})for(auto& vertex:*mesh){vertex.x+=e.x;vertex.y+=e.y;vertex.z+=e.z;}
    draw(celestial.sun,textures_.at("sun").id);draw(celestial.moon,textures_.at("moon_phases").id);
    glUniform1i(glGetUniformLocation(program_,"skyPass"),0);
    glEnable(GL_DEPTH_TEST);glEnable(GL_CULL_FACE);glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUniform1i(glGetUniformLocation(program_,"useLighting"),1);
    glUniform1f(glGetUniformLocation(program_,"fogDistance"),distance);
    glUniform3f(glGetUniformLocation(program_,"fogColor"),skyColour[0],skyColour[1],skyColour[2]);
    glBindVertexArray(vao_);glBindTexture(GL_TEXTURE_2D,textures_.at("terrain").id);
    if(frontSegmented_){
        for(const auto& chunk:frontChunks_)if(chunk.opaqueVertices){
            glBindBuffer(GL_ARRAY_BUFFER,chunk.opaque);attributes();glDrawArrays(GL_TRIANGLES,0,chunk.opaqueVertices);
        }
    }else{
        glBindBuffer(GL_ARRAY_BUFFER,terrainVbo_);attributes();glDrawArrays(GL_TRIANGLES,0,static_cast<GLsizei>(opaque_.size()));
    }
    draw(chests_,textures_.at("chest").id);
    draw(largeChests_,textures_.at("largechest").id);
    draw(enderChests_,textures_.at("enderchest").id);
    glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(-.3f,-.3f);
    draw(chestLids_,textures_.at("chest").id);draw(largeChestLids_,textures_.at("largechest").id);
    draw(enderChestLids_,textures_.at("enderchest").id);glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_CULL_FACE);
    for(int type=0;type<5;++type){
        const char* texture=type==0?"mob_skeleton":type==1?"mob_skeleton_wither":
            type==2?"mob_zombie":type==3?"mob_char":"mob_creeper";
        draw(skulls_[type],textures_.at(texture).id);
    }
    glEnable(GL_CULL_FACE);
    for(const auto& table:enchantTables_){
        const int x=table[0],y=table[1],z=table[2];
        const double dx=x+.5-eye.x,dy=y+.85-eye.y,dz=z+.5-eye.z;
        if(dx*dx+dy*dy+dz*dz>(distance+1)*(distance+1))continue;
        // The table list belongs to the last finished mesh; after the window
        // moves it can name cells the current world no longer holds.
        if(!source.inside(x,y,z) || source.get(x,y,z)!=static_cast<Block>(116))continue;
        const auto key=std::tuple{x,y,z};
        const auto bits=std::uint64_t(source.seed)+std::uint64_t(std::uint32_t(x))*341873128712ull+
                        std::uint64_t(std::uint32_t(z))*132897987541ull;
        auto [it,inserted]=bookAnimation_.try_emplace(key,std::bit_cast<std::int64_t>(bits));
        auto& animation=it->second;
        if(inserted || animation.lastWorldTick>source.time())animation.lastWorldTick=source.time();
        const Vec3 feet{eye.x,eye.y-1.62,eye.z};
        for(int step=0;animation.lastWorldTick<source.time() && step<40;++step){
            animation.tick(feet,x,y,z);++animation.lastWorldTick;
        }
        draw(buildBookMesh(x,y,z,animation,source.renderLight(x,y,z,false)),textures_.at("book").id);
    }
    // MobRenderer draws source ModelPart cubes without back-face culling.
    glDisable(GL_CULL_FACE);
    for(const auto& entity:source.entities()){
        const char* texture=entity.id==L"Zombie"?"mob_zombie":entity.id==L"PigZombie"?"mob_pigzombie":
            entity.id==L"Skeleton"?"mob_skeleton":entity.id==L"Spider"?"mob_spider":
            entity.id==L"CaveSpider"?"mob_cavespider":entity.id==L"Silverfish"?"mob_silverfish":
            entity.id==L"Villager"?"mob_villager":
            entity.id==L"Ozelot"?"mob_ozelot":
            entity.id==L"Wolf"?"mob_wolf":
            entity.id==L"Slime"?"mob_slime":entity.id==L"LavaSlime"?"mob_lava":
            entity.id==L"Ghast"?"mob_ghast":entity.id==L"Blaze"?"mob_blaze":
            entity.id==L"Squid"?"mob_squid":entity.id==L"Enderman"?"mob_enderman":
            entity.id==L"Creeper"?"mob_creeper":entity.id==L"Cow"?"mob_cow":
            entity.id==L"MushroomCow"?"mob_redcow":
            entity.id==L"Pig"?"mob_pig":entity.id==L"Sheep"?"mob_sheep":
            entity.id==L"Chicken"?"mob_chicken":nullptr;
        if(entity.id==L"Villager"){
            static constexpr std::array<const char*,5> professions{
                "mob_villager_farmer","mob_villager_librarian","mob_villager_priest",
                "mob_villager_smith","mob_villager_butcher"};
            if(entity.profession>=0 && entity.profession<int(professions.size()))
                texture=professions[entity.profession];
        }
        if(entity.id==L"Ozelot"){
            static constexpr std::array<const char*,4> cats{
                "mob_ozelot","mob_cat_black","mob_cat_red","mob_cat_siamese"};
            if(entity.catType>=0 && entity.catType<int(cats.size()))texture=cats[entity.catType];
        }
        if(entity.id==L"Wolf")
            texture=entity.wolfTame?"mob_wolf_tame":entity.wolfAngry?"mob_wolf_angry":"mob_wolf";
        if(!texture)continue;
        const double dx=entity.position.x-eye.x,dy=entity.position.y-eye.y,dz=entity.position.z-eye.z;
        if(dx*dx+dy*dy+dz*dz>distance*distance)continue;
        const int x=int(std::floor(entity.position.x)),y=int(std::floor(entity.position.y)),z=int(std::floor(entity.position.z));
        const int packedLight=source.inside(x,y,z)?source.renderLight(x,y,z,false):0;
        draw(buildMobMesh(entity,packedLight),textures_.at(texture).id);
        if(entity.id==L"MushroomCow")
            draw(buildMushroomCowMushrooms(entity,packedLight),textures_.at("terrain").id);
        if(entity.id==L"Sheep" && !entity.sheared)
            draw(buildMobMesh(entity,packedLight,true),textures_.at("mob_sheep_fur").id);
        if(entity.id==L"Wolf" && entity.wolfTame){
            glDepthFunc(GL_LEQUAL);
            draw(buildMobMesh(entity,packedLight,true),textures_.at("mob_wolf_collar").id);
            glDepthFunc(GL_LESS);
        }
        if(entity.id==L"Slime")
            draw(buildMobMesh(entity,packedLight,true),textures_.at("mob_slime").id);
        if(entity.id==L"Enderman"){
            glDepthFunc(GL_LEQUAL);
            glBlendFunc(GL_ONE,GL_ONE);
            draw(buildMobMesh(entity,packedLight,true),textures_.at("mob_enderman_eyes").id);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(GL_LESS);
        }
        if(entity.hurtTicks>0){
            auto hitOverlay=buildMobMesh(entity,packedLight);
            for(auto& vertex:hitOverlay){vertex.r=1;vertex.g=0;vertex.b=0;vertex.a=.4f;}
            glDepthFunc(GL_EQUAL);glDepthMask(GL_FALSE);
            draw(hitOverlay,white_);
            glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);
        }
    }
    for(const auto& orb:source.experienceOrbs()){
        const double dx=orb.position.x-eye.x,dy=orb.position.y-eye.y,dz=orb.position.z-eye.z;
        const double squared=dx*dx+dy*dy+dz*dz;
        if(squared<4 || squared>distance*distance)continue;
        const int x=int(std::floor(orb.position.x)),y=int(std::floor(orb.position.y)),
                  z=int(std::floor(orb.position.z));
        const int light=source.inside(x,y,z)?source.renderLight(x,y,z,false):0;
        draw(buildExperienceOrbMesh(orb,yaw,pitch,light),textures_.at("xporb").id);
    }
    for(const auto& item:source.droppedItems()){
        const double dx=item.position.x-eye.x,dy=item.position.y-eye.y,dz=item.position.z-eye.z;
        if(dx*dx+dy*dy+dz*dz>distance*distance)continue;
        const int x=int(std::floor(item.position.x)),y=int(std::floor(item.position.y)),z=int(std::floor(item.position.z));
        const auto mesh=buildDroppedItemMesh(item,yaw,pitch,source.inside(x,y,z)?source.renderLight(x,y,z,false):0);
        if(!mesh.terrain.empty())draw(mesh.terrain,textures_.at("terrain").id);
        if(!mesh.items.empty())draw(mesh.items,textures_.at("items").id);
    }
    for(const auto& decoration:source.hangingDecorations()){
        const double dx=decoration.tileX+.5-eye.x,dy=decoration.tileY+.5-eye.y,dz=decoration.tileZ+.5-eye.z;
        if(dx*dx+dy*dy+dz*dz>(distance+4)*(distance+4))continue;
        // Decorations stay listed while their chunk is in the streaming halo,
        // outside the rendered window where renderLight has no samples.
        if(!source.inside(decoration.tileX,decoration.tileY,decoration.tileZ))continue;
        const int packedLight=source.renderLight(decoration.tileX,decoration.tileY,decoration.tileZ,false);
        const auto mesh=buildHangingMesh(decoration,packedLight,[&](int x,int y,int z){
            return source.inside(x,y,z)?source.renderLight(x,y,z,false):packedLight;
        });
        draw(mesh.painting,textures_.at("art_kz").id);
        draw(mesh.frame,textures_.at("terrain").id);
        draw(mesh.itemTerrain,textures_.at("terrain").id);
        draw(mesh.itemAtlas,textures_.at("items").id);
    }
    glEnable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D,textures_.at("terrain").id);
    glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);
    if(frontSegmented_){
        for(const auto& chunk:frontChunks_)if(chunk.waterVertices){
            glBindBuffer(GL_ARRAY_BUFFER,chunk.water);attributes();glDrawArrays(GL_TRIANGLES,0,chunk.waterVertices);
        }
    }else{
        glBindBuffer(GL_ARRAY_BUFFER,waterVbo_);attributes();glDrawArrays(GL_TRIANGLES,0,static_cast<GLsizei>(water_.size()));
    }
    glDepthMask(GL_TRUE);
    glUniform1i(glGetUniformLocation(program_,"useLighting"),0);
    auto clouds=buildCloudMesh(eye,cloudTicks_,consoleCloudColour(source.time(),source.rainLevel(),source.thunderLevel()));
    glBindTexture(GL_TEXTURE_2D,textures_.at("clouds").id);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    draw(clouds,textures_.at("clouds").id);
    if(hit.hit){
        std::vector<Vertex> lines;glm::vec3 p(hit.x-.002f,hit.y-.002f,hit.z-.002f);
        for(int a=0;a<3;++a)for(int b=0;b<2;++b)for(int c=0;c<2;++c){glm::vec3 q=p;q[(a+1)%3]+=b*1.004f;q[(a+2)%3]+=c*1.004f;
            lines.push_back({q.x,q.y,q.z,0,0,0,0,0,.8f});q[a]+=1.004f;lines.push_back({q.x,q.y,q.z,0,0,0,0,0,.8f});}
        draw(lines,white_,GL_LINES);
        if(destroyStage_>=0 && source.inside(hit.x,hit.y,hit.z)){
            const int light=source.inside(hit.px,hit.py,hit.pz)?source.renderLight(hit.px,hit.py,hit.pz,false):0;
            glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(-1.f,-1.f);
            draw(buildDestroyStageMesh(hit.x,hit.y,hit.z,destroyStage_,light),textures_.at("terrain").id);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }
}
void Renderer::beginUI() {
    glUseProgram(program_);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUniform1i(glGetUniformLocation(program_,"useLighting"),0);
    glm::mat4 m=glm::ortho(0.f,uiWidth(),360.f,0.f,-1.f,1.f);
    glUniformMatrix4fv(glGetUniformLocation(program_,"matrix"),1,GL_FALSE,glm::value_ptr(m));
    glUniform1f(glGetUniformLocation(program_,"fogDistance"),0);
}
void Renderer::quad(GLuint texture,float x,float y,float w,float h,glm::vec4 uv,glm::vec4 color) {
    std::vector<Vertex> v;v.reserve(6);
    for(auto p:{glm::vec4(x,y,uv.x,uv.y),glm::vec4(x+w,y,uv.z,uv.y),glm::vec4(x+w,y+h,uv.z,uv.w),
        glm::vec4(x,y,uv.x,uv.y),glm::vec4(x+w,y+h,uv.z,uv.w),glm::vec4(x,y+h,uv.x,uv.w)})
        v.push_back({p.x,p.y,0,p.z,p.w,color.r,color.g,color.b,color.a});
    draw(v,texture);
}
void Renderer::rect(float x,float y,float w,float h,glm::vec4 color){quad(white_,x,y,w,h,{0,0,1,1},color);}
void Renderer::panel(float x,float y,float w,float h){
    // The PS3 XUI skin stretches the middle cells of its 32-pixel nine-slice
    // panel. UI coordinates here are half the original 1280x720 canvas.
    constexpr float edge=16;
    const float widths[3]={edge,std::max(0.f,w-2*edge),edge};
    const float heights[3]={edge,std::max(0.f,h-2*edge),edge};
    static constexpr const char* names[3][3]={{"panel_tl","panel_tm","panel_tr"},
        {"panel_ml","panel_mm","panel_mr"},{"panel_bl","panel_bm","panel_br"}};
    float yy=y;
    for(int row=0;row<3;++row){
        float xx=x;
        for(int col=0;col<3;++col){sprite(names[row][col],xx,yy,widths[col],heights[row]);xx+=widths[col];}
        yy+=heights[row];
    }
}
void Renderer::sprite(const std::string& name,float x,float y,float w,float h,glm::vec4 uv,glm::vec4 color){quad(textures_.at(name).id,x,y,w,h,uv,color);}
float Renderer::textWidth(const std::string& s,float scale)const {float w=0;for(unsigned char c:s)w+=glyphWidths_[c]*scale;return w;}
void Renderer::text(const std::string& s,float x,float y,float scale,glm::vec4 color,bool shadow) {
    if(shadow)text(s,x+scale,y+scale,scale,{color.r*.25f,color.g*.25f,color.b*.25f,color.a},false);
    std::vector<Vertex> v;v.reserve(s.size()*6);
    for(unsigned char c:s){float u=(c%16)/16.f,t=(c/16)/16.f;
        for(auto p:{glm::vec4(x,y,u,t),glm::vec4(x+8*scale,y,u+1/16.f,t),glm::vec4(x+8*scale,y+8*scale,u+1/16.f,t+1/16.f),
            glm::vec4(x,y,u,t),glm::vec4(x+8*scale,y+8*scale,u+1/16.f,t+1/16.f),glm::vec4(x,y+8*scale,u,t+1/16.f)})
            v.push_back({p.x,p.y,0,p.z,p.w,color.r,color.g,color.b,color.a});
        x+=glyphWidths_[c]*scale;
    }
    draw(v,textures_.at("font").id);
}
void Renderer::centered(const std::string& s,float y,float scale,glm::vec4 color){text(s,(uiWidth()-textWidth(s,scale))/2,y,scale,color);}
void Renderer::blockIcon(Block b,float x,float y,float size,int data) {
    if(b==static_cast<Block>(130)){
        sprite("enderchest",x,y+size*.27f,size,size*.73f,{14.f/64,33.f/64,28.f/64,43.f/64});
        sprite("enderchest",x,y,size,size*.3f,{14.f/64,15.f/64,28.f/64,20.f/64});
        return;
    }
    int tile=textureTile(b,2,std::clamp(data,0,15));glm::vec4 tint(1);if(b==Leaves)tint={.48,.76,.29,1};
    sprite("terrain",x,y,size,size,{(tile%16)/16.f,(tile/16)/16.f,(tile%16+1)/16.f,(tile/16+1)/16.f},tint);
}
void Renderer::prompt(char symbol,const std::string& label,float x,float y) {
    std::vector<Vertex> vertices;
    glm::vec4 color=symbol=='X'?glm::vec4(.45,.65,1,1):symbol=='O'?glm::vec4(1,.4,.4,1):glm::vec4(.35,1,.65,1);
    auto segment=[&](float ax,float ay,float bx,float by){
        glm::vec2 direction=glm::normalize(glm::vec2(bx-ax,by-ay));glm::vec2 side(-direction.y*.6f,direction.x*.6f);
        glm::vec2 a(x+ax,y+ay),b(x+bx,y+by);
        for(auto p:{a+side,b+side,b-side,a+side,b-side,a-side})vertices.push_back({p.x,p.y,0,0,0,color.r,color.g,color.b,color.a});
    };
    if(symbol=='X'){segment(1,1,7,7);segment(7,1,1,7);}
    else if(symbol=='O'){for(int i=0;i<24;++i){double a=i*PI/12,b=(i+1)*PI/12;segment(4+3.5*std::cos(a),4+3.5*std::sin(a),4+3.5*std::cos(b),4+3.5*std::sin(b));}}
    else {segment(4,0,8,7);segment(8,7,0,7);segment(0,7,4,0);}
    draw(vertices,white_);text(label,x+13,y);
}
void Renderer::screenshot(const std::filesystem::path& path) {
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    std::vector<unsigned char> pixels(width_*height_*4);glReadPixels(0,0,width_,height_,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    stbi_flip_vertically_on_write(1);
    if(!stbi_write_png(path.string().c_str(),width_,height_,4,pixels.data(),width_*4))throw std::runtime_error("Could not write screenshot");
}
}
