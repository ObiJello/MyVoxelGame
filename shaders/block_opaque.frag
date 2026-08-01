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
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1)
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off

// Output
out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard fully transparent pixels (grass side overlay, etc.)
    // Hardcoded constant lets GPU optimize better than a uniform threshold
    if (textureColor.a < 0.1) discard;

    // Vertex color contains: biome tint * AO * directional face shade
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    // Day/night sky-light dim + MC-style distance fog
    finalColor *= uSkyBrightness;
    vec3 fogDelta = fragWorldPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    finalColor = mix(finalColor, uFogColor.rgb, fogValue * uFogColor.a);

    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
