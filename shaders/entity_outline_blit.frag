#version 330 core
// File: shaders/entity_outline_blit.frag (OpenGL) — MC core/blit_screen.fsh:
// the finished outline laid over the frame (the pipeline blends it
// SRC_ALPHA / ONE_MINUS_SRC_ALPHA, MC's ENTITY_OUTLINE_BLIT).
in vec2 texCoord;
out vec4 fragColor;

uniform sampler2D uInSampler;   // MC InSampler

void main() {
    fragColor = texture(uInSampler, texCoord);
}
