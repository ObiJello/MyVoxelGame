// File: shaders/block.vert
#version 330 core

// Input vertex attributes (24-byte compact vertex: pos + uv + RGBA8 color)
layout (location = 0) in vec3 aPos;       // Vertex position (world-space)
layout (location = 1) in vec2 aTexCoord;  // Texture coordinates from atlas
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized by GL)

// Uniforms
uniform mat4 uMVP;  // Model-View-Projection matrix
// World-space clip plane for portal see-through rendering. Mirrors
// Portal's PushCustomClipPlane (portalrenderable_flatbasic.cpp:454).
// xyz = plane normal (must be unit), w = -dot(normal, point on plane).
// Fragments with positive distance are kept; negative = clipped.
// vec4(0) = no clipping (the dot product is then zero and -inf > 0 is
// always true).
uniform vec4 uPortalClipPlane;

// Output to fragment shader
out vec2 fragTexCoord;
out vec3 fragWorldPos;
out vec4 fragColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, aPos) + uPortalClipPlane.w
        : 1.0;
    fragTexCoord = aTexCoord;
    fragWorldPos = aPos;
    fragColor = aColor;
}
