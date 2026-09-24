// File: shaders/lines_vk.frag — MC rendertype_lines.fsh (no fog here).
#version 450

layout(location = 0) in vec4 fragColor;

// Descriptor set required by the shared pipeline layout (not sampled).
layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = fragColor;
}
