// File: shaders/clouds.vert (OpenGL twin of clouds_vk.vert) — CloudRenderer.
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec4 vColor;
out float vDist;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vDist = length((uModel * vec4(aPos, 1.0)).xyz);
}
