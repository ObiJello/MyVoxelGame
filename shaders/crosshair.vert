// File: shaders/crosshair.vert (OpenGL twin of crosshair_vk.vert) — Crosshair.
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 TexCoord;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    TexCoord = aTexCoord;
}
