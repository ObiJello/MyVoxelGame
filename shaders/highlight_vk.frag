#version 450

layout(location = 0) in vec4 fragColor;

// Descriptor set required by shared pipeline layout (not sampled)
layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = fragColor;
}
