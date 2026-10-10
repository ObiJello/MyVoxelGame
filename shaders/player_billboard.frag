// File: shaders/player_billboard.frag (OpenGL twin of player_billboard_vk.frag)
// uClipPlane: xyz = world-space plane normal, w = -dot(normal, point on
// plane); a pixel on the wrong side is discarded. (0,0,0,0) = no clipping.
#version 330 core
in vec3 vWorldPos;
in vec4 vColor;
out vec4 FragColor;

uniform vec4 uClipPlane;

void main() {
    if (any(notEqual(uClipPlane.xyz, vec3(0.0))) &&
        dot(vWorldPos, uClipPlane.xyz) + uClipPlane.w < 0.0) {
        discard;
    }
    FragColor = vColor;
}
