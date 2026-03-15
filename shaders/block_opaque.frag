// File: shaders/block_opaque.frag
// Opaque fragment shader with minimal alpha cutoff.
// Uses a hardcoded constant (not a uniform) so the GPU compiler can optimize
// the discard path aggressively — it fires only for truly transparent pixels
// (e.g., grass block side overlay regions). This is much cheaper than the
// cutout shader's uniform-based threshold which prevents early-z entirely.
#version 330 core

// Input from vertex shader
in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;

// Uniforms
uniform sampler2D uTextureAtlas;

// Output
out vec4 FragColor;

void main() {
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard fully transparent pixels (grass side overlay, etc.)
    // Hardcoded constant lets GPU optimize better than a uniform threshold
    if (textureColor.a < 0.1) discard;

    // Vertex color contains: biome tint * AO * directional face shade
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
