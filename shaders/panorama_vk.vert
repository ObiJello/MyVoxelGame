// File: shaders/panorama_vk.vert
//
// Title-screen panorama skybox face. Companion to PanoramaRenderer.{hpp,cpp}.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
} pc;

layout(location = 0) out vec2 vUV;

out gl_PerVertex { vec4 gl_Position; };

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = aUV;
}
