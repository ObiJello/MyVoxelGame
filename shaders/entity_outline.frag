#version 330 core
// File: shaders/entity_outline.frag (OpenGL) — MC rendertype_outline.fsh:
// every covered texel of the entity's texture becomes the team colour at
// full alpha; only fully transparent texels are left out.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSilhouetteTex;   // the entity's own texture
uniform vec4 uColor;   // rgb = team colour (MC OutlineBufferSource's vertex colour)

void main() {
    if (texture(uSilhouetteTex, vUV).a == 0.0) discard;
    FragColor = vec4(uColor.rgb, 1.0);
}
