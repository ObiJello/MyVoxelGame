// File: shaders/block_solid_vk.frag (Vulkan version of block_solid.frag)
// No-discard shader — ZERO discard, ZERO alpha testing.
// Used for translucent pass (water, ice, stained glass) where blending
// handles transparency. Full early-z and Hi-Z optimization enabled.
#version 450

// Input from vertex shader
layout (location = 0) in vec2 fragTexCoord;
layout (location = 1) in vec3 fragWorldPos;
layout (location = 2) in vec4 fragColor;

// Texture atlas sampler (descriptor set 0, binding 0)
layout (set = 0, binding = 0) uniform sampler2D uTextureAtlas;

// Push constants (must match C++ PushConstantBlock layout exactly)
layout (push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

// Output
layout (location = 0) out vec4 FragColor;

void main() {
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);
    vec3 finalColor = textureColor.rgb * fragColor.rgb;
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
