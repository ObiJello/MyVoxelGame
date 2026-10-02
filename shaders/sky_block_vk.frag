// File: shaders/sky_block_vk.frag (Vulkan version of sky_block.frag)
//
// Colour writes are off for this pass (sky_block.vert); the output only
// satisfies the pipeline's colour attachment.
#version 450

layout (location = 0) out vec4 FragColor;

void main() {
    FragColor = vec4(0.0);
}
