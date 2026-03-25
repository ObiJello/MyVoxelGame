#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec4 texColor = texture(uTexture, vTexCoord);
    FragColor = texColor * vColor;
}
