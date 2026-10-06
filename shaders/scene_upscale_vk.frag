#version 450
// File: shaders/scene_upscale_vk.frag
// Render Resolution's upscale: the scaled scene, sampled bilinearly (the
// sampler's filter — what vkCmdBlitImage's VK_FILTER_LINEAR did before).
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = texture(uScene, vUV);
}
