// File: shaders/sky_block.frag
//
// See sky_block.vert. The pass runs with colour writes disabled, so nothing
// here reaches the framebuffer; the output exists only so the program links.
#version 330 core

out vec4 FragColor;

void main() {
    FragColor = vec4(0.0);
}
