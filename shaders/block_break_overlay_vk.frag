// File: shaders/block_break_overlay_vk.frag
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

void main() {
    vec4 t = texture(uAtlas, vUV);
    // MC's "crumbling" blend pairs with blendFunc(DST_COLOR, SRC_COLOR).
    // Discard transparent pixels so they don't multiply-to-black.
    if (t.a < 0.05) discard;
    FragColor = vec4(t.rgb, 1.0);
}
