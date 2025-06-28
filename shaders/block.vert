#version 330 core

// Input vertex attributes
layout (location = 0) in vec3 aPos;       // Vertex position
layout (location = 1) in vec3 aNormal;    // Face normal
layout (location = 2) in vec2 aTexCoord;  // Texture coordinates from atlas

// Uniforms
uniform mat4 uMVP;  // Model-View-Projection matrix

// Output to fragment shader
out vec3 fragNormal;    // Interpolated normal for lighting
out vec2 fragTexCoord;  // Interpolated texture coordinates

void main() {
    // Transform vertex position to clip space
    gl_Position = uMVP * vec4(aPos, 1.0);

    // Pass through normal (assume no non-uniform scaling for now)
    fragNormal = aNormal;

    // Pass through texture coordinates
    fragTexCoord = aTexCoord;
}