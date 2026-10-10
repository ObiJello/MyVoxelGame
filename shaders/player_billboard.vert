// File: shaders/player_billboard.vert (OpenGL twin of player_billboard_vk.vert)
// Flat-coloured world geometry: the player's chat bubble (PlayerRenderer)
// and Gizmos' filled shapes. uModel is an optional pre-transform (identity
// normally; the portal pair matrix for a portal ghost pass).
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec3 vWorldPos;
out vec4 vColor;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos    = worldPos.xyz;
    gl_Position  = uMVP * worldPos;
    vColor       = aColor;
}
