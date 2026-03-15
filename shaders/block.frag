// File: shaders/block.frag
#version 330 core

// Input from vertex shader
in vec2 fragTexCoord;   // Interpolated texture coordinates
in vec3 fragWorldPos;   // World position
in vec4 fragColor;      // Vertex color (tint * AO * directional shade baked in)

// Uniforms
uniform sampler2D uTextureAtlas;     // The main texture atlas
uniform float uAlphaTest;           // Alpha discard threshold (per-pass)

// Output
out vec4 FragColor;

void main() {
    // Sample the texture atlas
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard transparent pixels (threshold varies per pass)
    if (textureColor.a < uAlphaTest) {
        discard;
    }

    // Vertex color already contains: biome tint * AO * directional face shade
    // This matches Minecraft's approach — all lighting is baked per-vertex
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    // Output final color with original alpha
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
