// File: shaders/entity_outline_blit_vk.frag (Vulkan twin of
// entity_outline_blit.frag — MC core/blit_screen.fsh).
#version 450

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

void main() {
    fragColor = texture(uTex, texCoord);
}
