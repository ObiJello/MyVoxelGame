// File: shaders/block.frag
#version 330 core

// Input from vertex shader
in vec2 fragTexCoord;   // Interpolated texture coordinates
in vec3 fragWorldPos;   // World position
in vec4 fragColor;      // Vertex color (tint * AO * directional shade baked in)

// Uniforms
uniform sampler2D uTextureAtlas;     // The main texture atlas
uniform float uAlphaTest;           // Alpha discard threshold (per-pass)
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1, MC SKY_LIGHT track)
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
    // Sample the texture atlas
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard transparent pixels (threshold varies per pass)
    if (textureColor.a < uAlphaTest) {
        discard;
    }

    // Vertex color already contains: biome tint * AO * directional face shade
    // This matches Minecraft's approach — all lighting is baked per-vertex
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    // Day/night sky-light dim (approximation of MC's lightmap night curve)
    finalColor *= uSkyBrightness;

    // MC fog.glsl: environmental fog on spherical distance + render-distance
    // fog on cylindrical distance, take the max.
    vec3 fogDelta = fragWorldPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    finalColor = mix(finalColor, uFogColor.rgb, fogValue * uFogColor.a);

    // Output final color with original alpha
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
