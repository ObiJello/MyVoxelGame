// File: shaders/terrain_solid.frag
// No-discard TERRAIN fragment shader: block_solid.frag plus the
// greedy-meshing tile-rect sample path. Used for the translucent pass
// (water, ice, stained glass) where blending handles transparency. NOTE the
// mesher never merges translucent quads (sort granularity), so the tiled
// branch is only ever taken here if that policy changes — it is kept so all
// three terrain fragment shaders share one vertex format and one contract.
// Keep in step with block_solid.frag apart from the sampling.
#version 330 core

in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;
in vec4 fragTileRect;   // xy = sprite origin, zw = sprite size; zw==0 = untiled

uniform sampler2D uTextureAtlas;
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1)
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off
// Debug fill override (greedy-mesh view): rgb painted at strength a over the
// final color. Zero (the GL default for an unset uniform) = passthrough.
uniform vec4 uOverlayColor;

out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

// See terrain_opaque.frag for the full rationale.
vec4 sampleTerrainAtlas() {
    vec2 tileSize = fragTileRect.zw;
    vec2 texDx = dFdx(fragTexCoord);
    vec2 texDy = dFdy(fragTexCoord);
    if (tileSize.x > 0.0) {
        vec2 atlasUV = fragTileRect.xy + fract(fragTexCoord) * tileSize;
        return textureGrad(uTextureAtlas, atlasUV, texDx * tileSize, texDy * tileSize);
    }
    return texture(uTextureAtlas, fragTexCoord);
}

void main() {

    vec4 textureColor = sampleTerrainAtlas();
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    // Day/night sky-light dim + MC-style distance fog
    finalColor *= uSkyBrightness;
    vec3 fogDelta = fragWorldPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    finalColor = mix(finalColor, uFogColor.rgb, fogValue * uFogColor.a);

    finalColor = mix(finalColor, uOverlayColor.rgb, uOverlayColor.a);
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
