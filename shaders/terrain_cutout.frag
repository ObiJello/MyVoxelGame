// File: shaders/terrain_cutout.frag
// Cutout TERRAIN fragment shader: block.frag plus the greedy-meshing
// tile-rect sample path (alpha-tested leaves, grass, the grass-side overlay
// quads). Keep in step with block.frag apart from the sampling — including
// the inverted-alpha uOverlayColor contract documented there (terrain never
// sets it; the all-zero default is a clean passthrough).
#version 330 core

// Input from vertex shader
in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;      // Vertex color (tint * AO * directional shade baked in)
in vec4 fragTileRect;   // xy = sprite origin, zw = sprite size; zw==0 = untiled

// Uniforms
uniform sampler2D uTextureAtlas;     // The main texture atlas
uniform float uAlphaTest;           // Alpha discard threshold (per-pass)
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1, MC SKY_LIGHT track)
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off
// MC's entity OVERLAY — see the long note in block.frag for why the alpha is
// inverted relative to vanilla (strength-from-zero, default vec4(0) = no-op).
uniform vec4 uOverlayColor;

// Output
out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

// See terrain_opaque.frag for the full rationale: fract() tiles a merged
// quad's sprite per block; textureGrad on the unwrapped uv's derivatives
// keeps mip selection seam-free; tileSize == 0 takes the exact pre-greedy
// texture() path.
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

    // Discard transparent pixels (threshold varies per pass)
    if (textureColor.a < uAlphaTest) {
        discard;
    }

    // Vertex color already contains: biome tint * AO * directional face shade
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    // MC entity.fsh order: overlay AFTER the vertex-colour multiply and
    // BEFORE the lightmap (see block.frag).
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

    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
