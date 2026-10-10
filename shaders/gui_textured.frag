// File: shaders/gui_textured.frag (OpenGL twin of gui_textured_vk.frag) — GuiRenderer.
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform float uAlphaTest;  // discard threshold; 0.0 means "discard only fully transparent pixels"
void main() {
    vec4 texColor = texture(uTexture, vTexCoord);
    // Mirrors gui_textured_vk.frag — see the comment there for why this
    // matters (lets the icon path write depth only on opaque pixels so the
    // glint pass can mask itself via depth-test EQUAL).
    if (texColor.a <= uAlphaTest) discard;
    FragColor = texColor * vColor;
}
