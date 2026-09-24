#version 330 core
// File: shaders/entity_outline_post.vert (OpenGL) — MC core/screenquad.vsh:
// the full-screen quad every entity-outline post pass draws. aPos is NDC,
// aUV the matching texture coordinate (0,0 bottom-left, GL's convention).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

out vec2 texCoord;

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    texCoord = aUV;
}
