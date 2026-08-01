// File: shaders/block_solid.frag
// No-discard fragment shader — ZERO discard, ZERO alpha testing.
// Used for the translucent pass (water, ice, stained glass) where blending
// handles transparency and discard is unnecessary. Also suitable for distant
// cutout LOD. Full GPU early-z and Hi-Z optimization enabled.
#version 330 core

in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;

uniform sampler2D uTextureAtlas;
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1)
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off

out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);
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
