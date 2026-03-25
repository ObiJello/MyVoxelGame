#version 450

layout(location = 1) in vec4 vColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = vColor;
}
