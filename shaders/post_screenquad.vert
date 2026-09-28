#version 330 core
// File: shaders/post_screenquad.vert (OpenGL) — MC core/screenquad.vsh: the
// full-screen quad every post pass draws (Render::PostChain). aPos is NDC,
// aUV the GL-convention texture coordinate.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

out vec2 texCoord;

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    texCoord = aUV;
}
