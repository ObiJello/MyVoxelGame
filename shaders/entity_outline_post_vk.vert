// File: shaders/entity_outline_post_vk.vert (Vulkan twin of
// entity_outline_post.vert). Every pass here draws with the frame's flipped
// viewport, which puts NDC +y at image row 0 — texture v = 0 — so v is the
// mirror of GL's, and the silhouettes, the passes and the composite all
// agree on which way is up.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec2 texCoord;

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    texCoord = vec2(aUV.x, 1.0 - aUV.y);
}
