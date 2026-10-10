// File: shaders/block_break_overlay.frag (OpenGL twin of block_break_overlay_vk.frag)
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAtlas;
void main() {
    vec4 t = texture(uAtlas, vUV);
    // MC's "crumbling" pass: blendFuncSeparate(DST_COLOR, SRC_COLOR, ONE, ZERO).
    // Formula: out.rgb = src.rgb * dst.rgb + dst.rgb * src.rgb = 2 * src * dst.
    // The destroy_stage_X.png is grey cracks on a transparent background, so:
    //   • At non-crack pixels (alpha = 0) we MUST discard — otherwise the
    //     2 * 0 * dst = 0 would punch a black hole in the block.
    //   • At crack pixels the grey RGB multiplicatively darkens the block,
    //     preserving its underlying texture/color (vs. drawing flat black).
    if (t.a < 0.05) discard;
    FragColor = vec4(t.rgb, 1.0);
}
