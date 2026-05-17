// File: shaders/portal_crosshair.vert
// OpenGL version — mirrors the inline GLSL in PortalCrosshair.cpp.
// Vulkan uses portal_crosshair_vk.{vert,frag} via path rewrite.
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
uniform vec2 uUVMin;
uniform vec2 uUVMax;

out vec2 vUV;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = mix(uUVMin, uUVMax, aUV);
}
