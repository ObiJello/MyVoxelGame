#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec4 texColor = texture(uTexture, vTexCoord);
    // Discard fragments below the alpha-test threshold. With uAlphaTest = 0
    // (the GUI default) ONLY fully-transparent pixels are discarded — visually
    // unchanged for normal rendering, but it lets the iso-block / sprite-icon
    // paths write depth ONLY where the icon is opaque. The enchanted-item
    // glint pass then runs with depth-test EQUAL to mask itself to exactly
    // the icon's silhouette (matching MC's BlendFunction.GLINT +
    // EQUAL_DEPTH_TEST trick — see RenderPipelines.GLINT line 197).
    if (texColor.a <= pc.uAlphaTest) discard;
    FragColor = texColor * vColor;
}
