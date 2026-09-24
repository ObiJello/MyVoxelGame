// Offscreen CGL context only: no window, client, image file or preview is opened.
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include "RendererShaders.h"
#include "TerrainMesh.h"
#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Context {
    CGLContextObj context=nullptr;
    Context(){
        CGLPixelFormatAttribute attributes[]={kCGLPFAOpenGLProfile,static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),static_cast<CGLPixelFormatAttribute>(0)};
        CGLPixelFormatObj format=nullptr;GLint count=0;
        auto error=CGLChoosePixelFormat(attributes,&format,&count);
        require(error==kCGLNoError && format && count>0,"No offscreen OpenGL 3.2 pixel format");
        error=CGLCreateContext(format,nullptr,&context);CGLDestroyPixelFormat(format);
        require(error==kCGLNoError && context,"Cannot create offscreen CGL context");
        if(CGLSetCurrentContext(context)!=kCGLNoError){CGLDestroyContext(context);context=nullptr;throw std::runtime_error("Cannot select offscreen CGL context");}
    }
    ~Context(){CGLSetCurrentContext(nullptr);if(context)CGLDestroyContext(context);}
};
static GLuint compile(GLenum type,const char* source){
    GLuint shader=glCreateShader(type);glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
    GLint okay=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&okay);
    if(!okay){char log[4096]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);glDeleteShader(shader);throw std::runtime_error(log);}
    return shader;
}
int main(){try{
    Context context;
    GLuint vertex=compile(GL_VERTEX_SHADER,console::terrainVertexShader),fragment=compile(GL_FRAGMENT_SHADER,console::terrainFragmentShader);
    GLuint program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);
    glBindAttribLocation(program,0,"position");glBindAttribLocation(program,1,"texcoord");glBindAttribLocation(program,2,"color");glBindAttribLocation(program,3,"lightcoord");
    glLinkProgram(program);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if(!linked){char log[4096]{};glGetProgramInfoLog(program,sizeof(log),nullptr,log);throw std::runtime_error(log);}
    glUseProgram(program);
    const float identity[]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    glUniformMatrix4fv(glGetUniformLocation(program,"matrix"),1,GL_FALSE,identity);
    glUniform3f(glGetUniformLocation(program,"eye"),0,0,0);glUniform1f(glGetUniformLocation(program,"fogDistance"),0);
    glUniform1i(glGetUniformLocation(program,"atlas"),0);glUniform1i(glGetUniformLocation(program,"lightmap"),1);
    GLuint framebuffer;glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    GLuint textures[3];glGenTextures(3,textures);glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,textures[0]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[0],0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Offscreen framebuffer incomplete");
    const std::array<unsigned char,4> atlas{200,100,50,255};
    glBindTexture(GL_TEXTURE_2D,textures[1]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,atlas.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    std::array<std::array<unsigned char,4>,256> light{};light[9*16+5]={128,64,255,255};
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,textures[2]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,light.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);
    const console::Vertex vertices[]={{-1,-1,0,.5,.5,1,1,1,1,5.5f/16,9.5f/16},{3,-1,0,.5,.5,1,1,1,1,5.5f/16,9.5f/16},{-1,3,0,.5,.5,1,1,1,1,5.5f/16,9.5f/16}};
    GLuint vao,buffer;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
    for(int i=0;i<4;++i)glEnableVertexAttribArray(i);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(console::Vertex),reinterpret_cast<void*>(offsetof(console::Vertex,x)));
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(console::Vertex),reinterpret_cast<void*>(offsetof(console::Vertex,u)));
    glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(console::Vertex),reinterpret_cast<void*>(offsetof(console::Vertex,r)));
    glVertexAttribPointer(3,2,GL_FLOAT,GL_FALSE,sizeof(console::Vertex),reinterpret_cast<void*>(offsetof(console::Vertex,lightU)));
    glViewport(0,0,2,2);glDisable(GL_DITHER);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);
    for(bool lighting:{true,false}){
        glUniform1i(glGetUniformLocation(program,"useLighting"),lighting);glDrawArrays(GL_TRIANGLES,0,3);
        std::array<unsigned char,4> pixel;glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        const std::array<int,4> expected=lighting?std::array<int,4>{100,25,50,255}:std::array<int,4>{200,100,50,255};
        for(int i=0;i<4;++i)require(std::abs(int(pixel[i])-expected[i])<=1,"Terrain lightmap or unlit UI shader output is incorrect");
    }
    const std::array<unsigned char,4> glow{200,100,50,10};
    glBindTexture(GL_TEXTURE_2D,textures[1]);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,glow.data());
    for(bool sky:{false,true}){
        glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(glGetUniformLocation(program,"skyPass"),sky);glDrawArrays(GL_TRIANGLES,0,3);
        std::array<unsigned char,4> pixel;glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        require(pixel==(sky?glow:std::array<unsigned char,4>{0,0,0,0}),"Sky preserves faint glow while terrain keeps alpha cutout");
    }
    require(glGetError()==GL_NO_ERROR,"Offscreen renderer shader raised an OpenGL error");
    glDeleteBuffers(1,&buffer);glDeleteVertexArrays(1,&vao);glDeleteTextures(3,textures);glDeleteFramebuffers(1,&framebuffer);glDeleteProgram(program);glDeleteShader(vertex);glDeleteShader(fragment);
    std::cout<<"Actual renderer shaders compiled, linked and passed offscreen lighting/UI pixel checks\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
