// File: shaders/gui_color.frag (OpenGL twin of gui_color_vk.frag) — GuiRenderer.
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
