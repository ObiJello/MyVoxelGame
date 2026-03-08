// File: shaders/block_vk.vert (Vulkan version)
#version 450

// Input vertex attributes (same layout as OpenGL version)
layout (location = 0) in vec3 aPos;       // Vertex position
layout (location = 1) in vec3 aNormal;    // Face normal
layout (location = 2) in vec2 aTexCoord;  // Texture coordinates from atlas
layout (location = 3) in vec4 aColor;     // Vertex color/tint

// Push constants (must match C++ PushConstantBlock layout exactly)
layout (push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

// Output to fragment shader
layout (location = 0) out vec3 fragNormal;
layout (location = 1) out vec2 fragTexCoord;
layout (location = 2) out vec3 fragWorldPos;
layout (location = 3) out vec4 fragColor;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    fragNormal = aNormal;
    fragTexCoord = aTexCoord;
    fragWorldPos = aPos;
    fragColor = aColor;
}
