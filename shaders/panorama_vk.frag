// File: shaders/panorama_vk.frag
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

void main() {
    FragColor = vec4(texture(uTexture, vUV).rgb, 1.0);
}
