// File: shaders/crosshair.frag (OpenGL twin of crosshair_vk.frag) — Crosshair.
#version 330 core

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    vec4 texColor = texture(uTexture, TexCoord);
    if (texColor.a < 0.5) discard;
    // Force alpha to 1.0 — OneMinusDstColor blending zeroes alpha against
    // an opaque framebuffer (1 - 1 = 0), making the crosshair invisible.
    // We only want RGB inversion; alpha must stay opaque.
    FragColor = vec4(texColor.rgb, 1.0);
}
