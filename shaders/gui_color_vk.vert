#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
} pc;

layout(location = 1) out vec4 vColor;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vColor = aColor;
}
