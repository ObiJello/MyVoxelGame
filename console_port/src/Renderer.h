#pragma once
#include "World.h"
#include "TerrainMesh.h"
#include "ConsoleLightmap.h"
#include "BookMesh.h"
#include "Random.h"
#include "TextureAnimation.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace console {
struct Texture { GLuint id=0; int width=0,height=0; };
class Renderer {
public:
    explicit Renderer(const std::filesystem::path& assets);
    ~Renderer();
    Renderer(const Renderer&)=delete;
    Renderer& operator=(const Renderer&)=delete;
    void resize(int width,int height);
    void rebuild(const World& world);
    void beginRebuild(const World& world);
    bool stepRebuild(const World& world,int chunkBudget=4);
    bool rebuilding()const{return pendingMesh_.has_value();}
    void uploadMesh(TerrainMesh mesh);
    void world(const World& source,Vec3 eye,double yaw,double pitch,double distance,const Hit& hit);
    void tickLighting(double seconds);
    void beginUI();
    void rect(float x,float y,float w,float h,glm::vec4 color);
    void panel(float x,float y,float w,float h);
    void sprite(const std::string& name,float x,float y,float w,float h,
        glm::vec4 uv={0,0,1,1},glm::vec4 color={1,1,1,1});
    void text(const std::string& value,float x,float y,float scale=1,glm::vec4 color={1,1,1,1},bool shadow=true);
    float textWidth(const std::string& value,float scale=1) const;
    void centered(const std::string& value,float y,float scale=1,glm::vec4 color={1,1,1,1});
    void blockIcon(Block block,float x,float y,float size,int data=0);
    void prompt(char symbol,const std::string& label,float x,float y);
    void screenshot(const std::filesystem::path& path);
    float uiWidth() const { return float(width_)*360/height_; }
    std::size_t triangleCount()const;
private:
    Texture load(const std::filesystem::path& path,bool smooth=false);
    void draw(const std::vector<Vertex>& vertices,GLuint texture,GLenum primitive=GL_TRIANGLES);
    void quad(GLuint texture,float x,float y,float w,float h,glm::vec4 uv,glm::vec4 color);
    void uploadAnimation(std::size_t index,int frame);
    struct ChunkBuffers {
        GLuint opaque=0,water=0;
        GLsizei opaqueVertices=0,waterVertices=0;
    };
    static void releaseChunkBuffers(std::array<ChunkBuffers,64>& chunks);
    struct AnimatedLiquid {
        LiquidAnimationSpec spec;
        TextureAnimation animation;
        std::vector<unsigned char> pixels;
    };
    std::vector<AnimatedLiquid> liquidAnimations_;
    GLuint program_=0,vao_=0,vbo_=0,white_=0,terrainVbo_=0,waterVbo_=0,lightmap_=0;
    LightFlicker flicker_;
    Random lightRandom_;
    double lightTickSeconds_=0;
    std::int32_t cloudTicks_=0;
    int width_=1280,height_=720;
    std::map<std::string,Texture> textures_;
    std::array<int,256> glyphWidths_{};
    std::vector<Vertex> opaque_,water_,chests_,chestLids_,largeChests_,largeChestLids_,enderChests_,enderChestLids_;
    std::array<std::vector<Vertex>,5> skulls_;
    std::vector<std::array<int,3>> enchantTables_;
    std::map<std::tuple<int,int,int>,BookAnimation> bookAnimation_;
    std::array<ChunkBuffers,64> frontChunks_{};
    bool frontSegmented_=false;
    struct PendingMesh {
        TerrainMesh mesh;
        std::array<ChunkBuffers,64> chunks{};
        std::vector<std::uint8_t> blocks;
        std::uint64_t revision=0;
        int originX=0,originZ=0,nextChunk=0;
        bool needsRestart=false;
        int restartCount=0;
    };
    std::optional<PendingMesh> pendingMesh_;
};
}
