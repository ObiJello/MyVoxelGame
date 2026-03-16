// File: shaders/block_vk.vert (Vulkan version)
// Matches OpenGL block.vert — 24-byte compact vertex: pos + uv + RGBA8 color
#version 450

// Input vertex attributes (24-byte compact vertex)
layout (location = 0) in vec3 aPos;       // Vertex position (world-space)
layout (location = 1) in vec2 aTexCoord;  // Texture coordinates from atlas
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized)

// Push constants (must match C++ PushConstantBlock layout exactly)
layout (push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

// Output to fragment shader
layout (location = 0) out vec2 fragTexCoord;
layout (location = 1) out vec3 fragWorldPos;
layout (location = 2) out vec4 fragColor;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    fragTexCoord = aTexCoord;
    fragWorldPos = aPos;
    fragColor = aColor;
}
