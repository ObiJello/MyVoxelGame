// File: shaders/block_opaque_vk.frag (Vulkan version of block_opaque.frag)
// Opaque shader — hardcoded 0.1 discard constant for early-z optimization.
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

    // Discard fully transparent pixels (grass side overlay, etc.)
    // Hardcoded constant lets GPU optimize better than a push constant threshold
    if (textureColor.a < 0.1) discard;

    // Vertex color contains: biome tint * AO * directional face shade
    vec3 finalColor = textureColor.rgb * fragColor.rgb;
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
