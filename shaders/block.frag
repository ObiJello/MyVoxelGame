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

// MC's entity OVERLAY (OverlayTexture) — the white flash on primed TNT and the
// red flash on a hurt mob. rgb is the overlay colour; a is its STRENGTH.
//
// ALPHA IS INVERTED relative to vanilla, deliberately. MC samples a 16x16
// overlay texture and does `mix(overlay.rgb, color.rgb, overlay.a)`, where a=1
// means "no overlay" — its NO_OVERLAY texel is opaque white. An unset GL
// uniform is all zeroes, so keeping that convention would make every block in
// the world render BLACK the moment this uniform existed. Strength-from-zero is
// the same maths read the other way round:
//     mix(overlay, color, a)  ==  mix(color, overlay, 1 - a)
// so a caller converts with `strength = 1 - overlayTexel.a` and the default
// vec4(0) is a clean passthrough.
uniform vec4 uOverlayColor;

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

    // MC entity.fsh applies the overlay HERE — after the vertex-colour
    // multiply and BEFORE the lightmap. The position matters: an overlaid
    // block still dims at night and still fogs with distance, so a flashing
    // TNT in a dark cave is not a floating white square.
    finalColor = mix(finalColor, uOverlayColor.rgb, uOverlayColor.a);

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
