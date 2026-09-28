#version 450
// File: shaders/post_screenquad_vk.vert (Vulkan twin of post_screenquad.vert
// — MC core/screenquad.vsh). texCoord stays GL-convention: the post
// fragment shaders flip v when they sample (see post_blit_vk.frag).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec2 texCoord;

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    texCoord = aUV;
}
