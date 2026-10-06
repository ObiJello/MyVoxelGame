#version 450
// File: shaders/scene_upscale_vk.vert
// Render Resolution's upscale (VKBackend::ResolveScaledScene): one triangle
// over the whole swapchain image, no vertex input. uv (0,0) is the scene's
// first row, drawn at the top — both images are stored top row first.
layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
