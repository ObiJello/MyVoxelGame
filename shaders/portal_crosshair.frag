// File: shaders/portal_crosshair.frag
// OpenGL version — mirrors the inline GLSL in PortalCrosshair.cpp.
// Vulkan uses portal_crosshair_vk.{vert,frag} via path rewrite.
#version 330 core

uniform sampler2D uAtlas;
uniform vec4      uTint;

in  vec2 vUV;
out vec4 FragColor;

void main() {
    vec4 t = texture(uAtlas, vUV);
    FragColor = vec4(uTint.rgb * t.rgb, uTint.a * t.a);
}
