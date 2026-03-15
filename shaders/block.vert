// File: shaders/block.vert
#version 330 core

// Input vertex attributes (24-byte compact vertex: pos + uv + RGBA8 color)
layout (location = 0) in vec3 aPos;       // Vertex position (world-space)
layout (location = 1) in vec2 aTexCoord;  // Texture coordinates from atlas
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized by GL)

// Uniforms
uniform mat4 uMVP;  // Model-View-Projection matrix

// Output to fragment shader
out vec2 fragTexCoord;
out vec3 fragWorldPos;
out vec4 fragColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    fragTexCoord = aTexCoord;
    fragWorldPos = aPos;
    fragColor = aColor;
}
