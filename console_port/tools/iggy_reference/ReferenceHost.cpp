// Original Iggy 1.2.30 / GDraw reference host. No visible window is created.
// JSON lines on stdin/stdout; a render reply may be followed by raw RGBA bytes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "iggy.h"
#include "gdraw_wgl.h"
#include "UIFontData.h"
#include "UIBitmapFont.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

using Json=nlohmann::json;
static Json callbacks=Json::array(), warnings=Json::array(), customDraws=Json::array();

static Json valueJson(const IggyDataValue& v) {
    switch (v.type) {
    case IGGY_DATATYPE_boolean: return bool(v.boolval);
    case IGGY_DATATYPE_number: return v.number;
    case IGGY_DATATYPE_string_UTF8: return std::string(v.string8.string, v.string8.length);
    case IGGY_DATATYPE_string_UTF16: {
        int n=WideCharToMultiByte(CP_UTF8,0,v.string16.string,v.string16.length,nullptr,0,nullptr,nullptr);
        std::string out(n,'\0');
        WideCharToMultiByte(CP_UTF8,0,v.string16.string,v.string16.length,out.data(),n,nullptr,nullptr);
        return out;
    }
    default: return nullptr;
    }
}
static void RADLINK warning(void*,Iggy*,IggyResult code,const char* message) {
    warnings.push_back({{"code",int(code)},{"message",message}});
}
static rrbool RADLINK external(void*,Iggy*,IggyExternalFunctionCallUTF8* call) {
    Json args=Json::array();
    for(int i=0;i<call->num_arguments;++i) args.push_back(valueJson(call->arguments[i]));
    callbacks.push_back({{"name",std::string(call->function_name.string,call->function_name.length)},{"args",args}});
    return true;
}
static void RADLINK customDraw(void*,Iggy*,IggyCustomDrawCallbackRegion* region) {
    std::string name;
    for (auto* s=region->name;s && *s;++s) name+=char(*s);
    customDraws.push_back({{"name",name},{"bounds",{region->x0,region->y0,region->x1,region->y1}}});
}
static std::vector<unsigned char> readFile(const std::string& name) {
    std::ifstream f(name,std::ios::binary|std::ios::ate);
    if(!f) throw std::runtime_error("Cannot read "+name);
    auto size=f.tellg();
    if(size<=0 || size>128*1024*1024) throw std::runtime_error("Invalid file size: "+name);
    std::vector<unsigned char> out(static_cast<size_t>(size));
    f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),out.size());
    if(!f) throw std::runtime_error("Short read: "+name);
    return out;
}
static void check(bool ok,const char* what) {
    if(!ok) throw std::runtime_error(what);
}

class Offscreen {
    HWND window_=nullptr;
    HDC dc_=nullptr;
    HGLRC context_=nullptr;
    GLuint fbo_=0, color_=0, depth_=0;
    PFNGLBINDFRAMEBUFFEREXTPROC bind_=nullptr;
public:
    static constexpr int width=1280,height=720;
    Offscreen() {
        WNDCLASSA wc={};
        wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(nullptr);
        wc.lpszClassName="ConsoleIggyReference";wc.style=CS_OWNDC;
        check(RegisterClassA(&wc)!=0,"RegisterClass");
        // WS_VISIBLE is deliberately absent. Drawing is exclusively into an FBO.
        window_=CreateWindowExA(0,wc.lpszClassName,"",WS_POPUP,0,0,width,height,nullptr,nullptr,wc.hInstance,nullptr);
        check(window_!=nullptr,"Hidden OpenGL window");
        dc_=GetDC(window_);
        PIXELFORMATDESCRIPTOR pfd={};
        pfd.nSize=sizeof(pfd);pfd.nVersion=1;
        pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
        pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=32;pfd.cAlphaBits=8;pfd.cDepthBits=24;pfd.cStencilBits=8;
        int pf=ChoosePixelFormat(dc_,&pfd);
        check(pf && SetPixelFormat(dc_,pf,&pfd),"OpenGL pixel format");
        context_=wglCreateContext(dc_);
        check(context_ && wglMakeCurrent(dc_,context_),"OpenGL context");
#define LOAD_GL(type,name) auto name=reinterpret_cast<type>(wglGetProcAddress(#name));check(name!=nullptr,#name)
        LOAD_GL(PFNGLGENFRAMEBUFFERSEXTPROC,glGenFramebuffersEXT);
        LOAD_GL(PFNGLBINDFRAMEBUFFEREXTPROC,glBindFramebufferEXT);
        LOAD_GL(PFNGLFRAMEBUFFERTEXTURE2DEXTPROC,glFramebufferTexture2DEXT);
        LOAD_GL(PFNGLGENRENDERBUFFERSEXTPROC,glGenRenderbuffersEXT);
        LOAD_GL(PFNGLBINDRENDERBUFFEREXTPROC,glBindRenderbufferEXT);
        LOAD_GL(PFNGLRENDERBUFFERSTORAGEEXTPROC,glRenderbufferStorageEXT);
        LOAD_GL(PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC,glFramebufferRenderbufferEXT);
        LOAD_GL(PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC,glCheckFramebufferStatusEXT);
#undef LOAD_GL
        bind_=glBindFramebufferEXT;
        glGenFramebuffersEXT(1,&fbo_);bind_(GL_FRAMEBUFFER_EXT,fbo_);
        glGenTextures(1,&color_);glBindTexture(GL_TEXTURE_2D,color_);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,color_,0);
        glGenRenderbuffersEXT(1,&depth_);glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,depth_);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_DEPTH24_STENCIL8_EXT,width,height);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_DEPTH_ATTACHMENT_EXT,GL_RENDERBUFFER_EXT,depth_);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_STENCIL_ATTACHMENT_EXT,GL_RENDERBUFFER_EXT,depth_);
        check(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT,"OpenGL framebuffer");
    }
    ~Offscreen() {
        if(context_){wglMakeCurrent(nullptr,nullptr);wglDeleteContext(context_);}
        if(dc_)ReleaseDC(window_,dc_);
        if(window_)DestroyWindow(window_);
    }
    void begin() {
        bind_(GL_FRAMEBUFFER_EXT,fbo_);
        glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_TRUE);glStencilMask(255);
        glClearColor(0,0,0,0);glClearStencil(255);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
        gdraw_GL_SetTileOrigin(0,0,fbo_);
    }
    std::vector<unsigned char> readPixels() {
        gdraw_GL_NoMoreGDrawThisFrame();bind_(GL_FRAMEBUFFER_EXT,fbo_);
        std::vector<unsigned char> out(width*height*4);
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,out.data());
        check(glGetError()==GL_NO_ERROR,"OpenGL render/readback error");
        const int row=width*4;
        for(int y=0;y<height/2;++y)
            std::swap_ranges(out.begin()+y*row,out.begin()+(y+1)*row,out.begin()+(height-y-1)*row);
        return out;
    }
};

// Keep every parent alive while resolving nested IggyValuePaths.
struct ValuePath {
    std::vector<IggyValuePath> parents;
    IggyValuePath* value;
    ValuePath(Iggy* player,const std::string& path):value(IggyPlayerRootPath(player)) {
        parents.reserve(std::count(path.begin(),path.end(),'.')+1);
        size_t start=0;
        while(start<path.size()) {
            auto end=path.find('.',start);
            auto name=path.substr(start,end==std::string::npos?end:end-start);
            parents.emplace_back();
            check(IggyValuePathMakeNameRef(&parents.back(),value,name.c_str()),"Unknown movie object");
            value=&parents.back();
            if(end==std::string::npos)break;
            start=end+1;
        }
    }
};

class Host {
    Offscreen surface_;
    std::map<std::string,Iggy*> movies_;
    std::vector<IggyLibrary> libraries_;
    std::unique_ptr<UIBitmapFont> font7_,font11_;
public:
    Host() {
        IggyInit(nullptr);IggySetWarningCallback(warning,nullptr);
        IggySetAS3ExternalFunctionCallbackUTF8(external,nullptr);
        IggySetCustomDrawCallback(customDraw,nullptr);
        gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_vertexbuffer,5000,16*1024*1024);
        gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_texture,5000,128*1024*1024);
        gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_rendertarget,20,64*1024*1024);
        auto* functions=gdraw_GL_CreateContext(Offscreen::width,Offscreen::height,1);
        check(functions!=nullptr,"Original GDraw initialization");
        IggySetGDraw(functions);
        font7_=std::make_unique<UIBitmapFont>(SFontData::Mojangles_7);font7_->registerFont();
        font11_=std::make_unique<UIBitmapFont>(SFontData::Mojangles_11);font11_->registerFont();
        // UIController::loadSkins, PS3 branch and original dependency order.
        loadLibrary("platformskin.swf","PS3Media/Media/skinPS3.swf");
        for(const char* name:{"skinGraphics.swf","skinGraphicsHud.swf","skinGraphicsInGame.swf",
            "skinGraphicsTooltips.swf","skinGraphicsLabels.swf","skinLabels.swf","skinInGame.swf",
            "skinHud.swf","skinTooltips.swf","skin.swf"})
            loadLibrary(name,std::string("Common/Media/")+name);
    }
    ~Host() {
        for(auto& item:movies_)IggyPlayerDestroy(item.second);
        for(auto it=libraries_.rbegin();it!=libraries_.rend();++it)IggyLibraryDestroy(*it);
        font11_.reset();font7_.reset();
        // GDraw destroys Iggy-owned cached resources; it must precede Shutdown.
        gdraw_GL_DestroyContext();IggyShutdown();
    }
    void loadLibrary(const char* alias,const std::string& relative) {
        auto b=readFile("source_full/Minecraft.Client/"+relative);
        auto id=IggyLibraryCreateFromMemory(alias,b.data(),U32(b.size()),nullptr);
        check(id!=IGGY_INVALID_LIBRARY,"Original Iggy library load");libraries_.push_back(id);
    }
    Iggy* movie(const Json& request) {
        auto it=movies_.find(request.at("slot").get<std::string>());
        if(it==movies_.end())throw std::runtime_error("Unknown movie slot");
        return it->second;
    }
    Json execute(const Json& request,std::vector<unsigned char>& pixels) {
        const auto op=request.at("op").get<std::string>();
        if(op=="load") {
            auto slot=request.at("slot").get<std::string>();
            auto name=request.at("movie").get<std::string>();
            check(name.find('/')==std::string::npos && name.find('\\')==std::string::npos,"Movie must be an archive filename");
            auto b=readFile("source_full/Minecraft.Client/Common/Media/"+name);
            Iggy* player=IggyPlayerCreateFromMemory(b.data(),U32(b.size()),nullptr);
            check(player!=nullptr,"Movie creation");
            IggyPlayerInitializeAndTickRS(player);
            if(!IggyPlayerGetValid(player)){IggyPlayerDestroy(player);throw std::runtime_error("Movie initialization");}
            IggyPlayerSetDisplaySize(player,Offscreen::width,Offscreen::height);
            auto it=movies_.find(slot);if(it!=movies_.end())IggyPlayerDestroy(it->second);
            movies_[slot]=player;
            auto* p=IggyPlayerProperties(player);
            return {{"width",p->movie_width_in_pixels},{"height",p->movie_height_in_pixels},{"fps",p->movie_frame_rate_from_file_in_fps}};
        }
        if(op=="unload") {
            auto slot=request.at("slot").get<std::string>();
            IggyPlayerDestroy(movie(request));movies_.erase(slot);return {};
        }
        if(op=="call") {
            auto* player=movie(request);
            ValuePath path(player,request.value("path",""));
            auto args=request.value("args",Json::array());
            check(args.is_array() && args.size()<=64,"Invalid argument list");
            std::vector<std::string> strings;strings.reserve(args.size());
            std::vector<IggyDataValue> values(args.size());
            for(size_t i=0;i<args.size();++i) {
                auto& v=values[i];
                if(args[i].is_boolean()){v.type=IGGY_DATATYPE_boolean;v.boolval=args[i].get<bool>();}
                else if(args[i].is_number()){v.type=IGGY_DATATYPE_number;v.number=args[i].get<double>();}
                else if(args[i].is_string()){
                    strings.push_back(args[i].get<std::string>());v.type=IGGY_DATATYPE_string_UTF8;
                    v.string8={strings.back().data(),S32(strings.back().size())};
                } else if(args[i].is_null())v.type=IGGY_DATATYPE_null;
                else throw std::runtime_error("Unsupported argument type");
            }
            auto method=request.at("method").get<std::string>();
            auto name=IggyPlayerCreateFastNameUTF8(player,method.c_str(),-1);
            IggyDataValue result={};
            auto status=IggyPlayerCallMethodRS(player,&result,path.value,name,S32(values.size()),values.data());
            check(status==IGGY_RESULT_SUCCESS,"AS3 method call failed");
            return {{"type",result.type},{"value",valueJson(result)}};
        }
        if(op=="get") {
            ValuePath path(movie(request),request.value("path",""));
            auto property=request.at("property").get<std::string>();
            IggyDatatype type;
            check(IggyValueGetTypeRS(path.value,0,property.c_str(),&type)==0,"Unknown property");
            if(type==IGGY_DATATYPE_number){
                double n;check(IggyValueGetF64RS(path.value,0,property.c_str(),&n)==0,"Read number");
                return {{"value",n}};
            }
            if(type==IGGY_DATATYPE_boolean){
                rrbool b;check(IggyValueGetBooleanRS(path.value,0,property.c_str(),&b)==0,"Read boolean");
                return {{"value",bool(b)}};
            }
            if(type==IGGY_DATATYPE_string_UTF8 || type==IGGY_DATATYPE_string_UTF16){
                char text[65536];S32 length=0;
                check(IggyValueGetStringUTF8RS(path.value,0,property.c_str(),sizeof(text),text,&length)==0,"Read string");
                return {{"value",std::string(text,length)}};
            }
            return {{"type",int(type)},{"value",nullptr}};
        }
        if(op=="key") {
            IggyEvent event;IggyEventResult result;
            IggyMakeEventKey(&event,request.value("down",true)?IGGY_KEYEVENT_Down:IGGY_KEYEVENT_Up,
                IggyKeycode(request.at("code").get<int>()),IGGY_KEYLOC_Standard);
            IggyPlayerDispatchEventRS(movie(request),&event,&result);return {};
        }
        if(op=="tick") {
            auto* player=movie(request);
            double seconds=request.at("seconds").get<double>();
            check(std::isfinite(seconds) && seconds>=0,"Invalid timeline time");
            IggyPlayerDebugSetTime(player,seconds);IggyPlayerTickRS(player);
            check(IggyPlayerGetValid(player),"Movie invalid after tick");
            return {{"frames",IggyPlayerProperties(player)->frames_passed}};
        }
        if(op=="render") {
            surface_.begin();
            for(const auto& slot:request.at("slots")){
                auto* p=movie(Json{{"slot",slot}});
                IggyPlayerDraw(p);check(IggyPlayerGetValid(p),"Movie invalid after draw");
            }
            pixels=surface_.readPixels();
            size_t visible=0;for(size_t i=3;i<pixels.size();i+=4)visible+=pixels[i]!=0;
            if(!request.value("pixels",false))pixels.clear();
            return {{"width",Offscreen::width},{"height",Offscreen::height},{"format","RGBA8-top-down-premultiplied"},
                {"bytes",pixels.size()},{"nontransparent",visible},{"custom_draws",customDraws}};
        }
        throw std::runtime_error("Unknown operation");
    }
};

int main() {
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try {
        Host host;
        std::cout<<Json{{"ready",true},{"iggy",IggyVersion},{"protocol",1},{"warnings",warnings},
            {"renderer",reinterpret_cast<const char*>(glGetString(GL_RENDERER))}}.dump()<<std::endl;
        std::string line;
        while(std::getline(std::cin,line)){
            warnings=Json::array();callbacks=Json::array();customDraws=Json::array();
            Json reply;std::vector<unsigned char> pixels;
            try {
                auto request=Json::parse(line);
                if(request.value("op","")=="quit"){std::cout<<"{\"ok\":true}"<<std::endl;break;}
                reply=host.execute(request,pixels);reply["ok"]=true;
            } catch(const std::exception& e){reply={{"ok",false},{"error",e.what()}};pixels.clear();}
            reply["callbacks"]=callbacks;reply["warnings"]=warnings;
            std::cout<<reply.dump()<<'\n';
            if(!pixels.empty())std::cout.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
            std::cout.flush();
        }
        return 0;
    } catch(const std::exception& e) {
        std::cout<<Json{{"ready",false},{"error",e.what()},{"warnings",warnings}}.dump()<<std::endl;
        return 1;
    }
}
