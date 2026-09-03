// File: shaders/terrain_opaque.frag
// Opaque (solid-layer) TERRAIN fragment shader: block_opaque.frag plus the
// greedy-meshing tile-rect sample path. NO discard, NO alpha test — every
// fragment lands, alpha ignored, so early-z stays on and the pass runs at
// pure fill cost. The mesher-enforced invariant from block_opaque.frag holds
// unchanged: nothing in the opaque layer relies on transparent texels being
// dropped. Keep in step with block_opaque.frag apart from the sampling.
#version 330 core

// Input from vertex shader
in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;
in vec4 fragTileRect;   // xy = sprite origin, zw = sprite size; zw==0 = untiled

// Uniforms
uniform sampler2D uTextureAtlas;
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1)
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off
// Debug fill override (greedy-mesh view): rgb painted at strength a over the
// final color. Zero (the GL default for an unset uniform) = passthrough.
uniform vec4 uOverlayColor;

// Output
out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

// Greedy-merged quads tile their sprite: uv is in tile space (0..N block
// repeats) and the sprite's atlas rect rides the vertex. fract() folds each
// repeat back onto the sprite; textureGrad with the UNWRAPPED uv's
// derivatives (scaled into atlas space) keeps mip selection identical to an
// unmerged quad — fract()'s sawtooth would otherwise spike dFdx at every
// block seam and drop those pixel columns onto the smallest mip. Unmerged
// quads (tileSize == 0) take the plain texture() path so their output stays
// bit-identical to the pre-greedy shader. Derivatives are computed before
// the branch; the branch itself is uniform per primitive (all four corners
// carry the same tile rect).
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

    finalColor = mix(finalColor, uOverlayColor.rgb, uOverlayColor.a);
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
