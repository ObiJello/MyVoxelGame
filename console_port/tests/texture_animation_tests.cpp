#include "TextureAnimation.h"
#include "World.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F action){try{action();}catch(const std::invalid_argument&){return;}throw std::runtime_error("Expected invalid animation rejection");}
int main(){try{
    using namespace console;
    auto specs=liquidAnimationSpecs();
    for(auto name:{"sun","moon_phases","clouds"}){
        auto path=std::filesystem::path(CONSOLE_ASSET_DIR)/(std::string(name)+".png");
        int w,h,n;auto pixels=stbi_load(path.string().c_str(),&w,&h,&n,4);
        require(pixels!=nullptr,"Original sky asset decodes");stbi_image_free(pixels);
        int expectedWidth=std::string(name)=="sun"?32:std::string(name)=="moon_phases"?128:256;
        int expectedHeight=std::string(name)=="moon_phases"?64:expectedWidth;
        require(w==expectedWidth && h==expectedHeight,"Original sky texture and phase-atlas dimensions");
    }
    {
        auto path=std::filesystem::path(CONSOLE_ASSET_DIR)/"terrain.png";
        int w=0,h=0,n=0;
        auto pixels=stbi_load(path.string().c_str(),&w,&h,&n,4);
        require(pixels!=nullptr,"Terrain atlas decodes");
        require(w==256 && h==256,"Original terrain atlas dimensions");
        // The base archive's terrain.png has magenta placeholders at these
        // later Title Update registrations. These selectors must use actual
        // source textures or stairs and Nether fences render bright pink.
        const console::Block shaped[]={console::Fence,console::NetherFence,
            static_cast<console::Block>(53),static_cast<console::Block>(67),
            static_cast<console::Block>(108),static_cast<console::Block>(109),
            static_cast<console::Block>(114),static_cast<console::Block>(128),
            static_cast<console::Block>(134),static_cast<console::Block>(135),
            static_cast<console::Block>(136),static_cast<console::Block>(156)};
        for(auto block:shaped)for(int face=0;face<6;++face){
            const int tile=console::textureTile(block,face);
            require(tile>=0 && tile<256,"Shaped block selects a valid terrain tile");
            int magenta=0;
            for(int y=0;y<16;++y)for(int x=0;x<16;++x){
                const auto* pixel=pixels+4*((tile/16*16+y)*w+tile%16*16+x);
                if(pixel[0]==214 && pixel[1]==127 && pixel[2]==255 && pixel[3]==255)++magenta;
            }
            require(magenta<128,"Shaped block texture is not the old magenta placeholder");
        }
        stbi_image_free(pixels);
    }
    require(specs.size()==4,"Four original liquid animations");
    for(const auto& spec:specs){
        int w,h,n;auto path=std::filesystem::path(CONSOLE_ASSET_DIR)/"animations"/(std::string(spec.name)+".png");
        auto pixels=stbi_load(path.string().c_str(),&w,&h,&n,4);
        require(pixels!=nullptr,"Original strip decodes");
        bool shape=w==spec.size && h==spec.size*spec.frames;stbi_image_free(pixels);
        require(shape,"Original vertical square-frame strip dimensions");
        require(spec.x>=0 && spec.y>=0 && spec.x+spec.size<=256 && spec.y+spec.size<=256,"Atlas upload bounds");
        TextureAnimation animation(spec.frames,spec.schedule);
        int displayed=0;
        for(int tick=1;tick<=2000;++tick){
            int expected=0;
            if(spec.name=="water")expected=(tick/2)%32;
            if(spec.name=="water_flow")expected=tick%32;
            if(spec.name=="lava_flow")expected=(tick/3)%16;
            if(spec.name=="lava"){int phase=(tick/2)%38;expected=phase<=19?phase:38-phase;}
            int upload=animation.tick();
            require(upload==(expected==displayed?-1:expected),"Exact original frame duration, reversal and upload suppression");
            if(upload>=0)displayed=upload;
        }
    }
    TextureAnimation repeated(3,{{0,1},{0,2},{2,1}});
    require(repeated.tick()==-1 && repeated.tick()==-1 && repeated.tick()==2 && repeated.tick()==0,"Repeated frame skips redundant upload");
    TextureAnimation single(1);require(single.tick()==-1 && single.tick()==-1,"Static loaded texture never reuploads");
    rejects([]{TextureAnimation a(0);});rejects([]{TextureAnimation a(2,{{2,1}});});
    rejects([]{TextureAnimation a(2,{{0,0}});});rejects([]{TextureAnimation a(2,{{-1,1}});});
    rejects([]{TextureAnimation a(2,{{0,1000001}});});
    rejects([]{TextureAnimation a(2,std::vector<std::pair<int,int>>(600,{0,1}));});
    std::cout<<"Original liquid assets and 8000 animation ticks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
