// File: shaders/sky.vert (OpenGL twin of sky_vk.vert)
// The sky dome, sun, moon and stars: SkyRenderer. Drawn after the opaque
// and cutout terrain at the far plane (depth 1.0, LessEqual), so no sky
// fragment the terrain covers is shaded — see sky_vk.vert.
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;
out float vSph;
out float vCyl;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    // The far plane (depth 1.0, the clear) — see shaders/sky_vk.vert.
    gl_Position.z = gl_Position.w;
    vUV = aUV;
    vColor = aColor;
    // MC sky.vsh: fog distances from the RAW buffer position (the sky is
    // camera-centered, so model-space distance == camera distance).
    vSph = length(aPos);
    vCyl = max(length(aPos.xz), abs(aPos.y));
}
