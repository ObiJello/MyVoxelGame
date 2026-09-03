// File: shaders/terrain_opaque_vk.frag (Vulkan twin of terrain_opaque.frag)
// Opaque (solid-layer) TERRAIN fragment shader: block_opaque_vk.frag plus the
// greedy-meshing tile-rect sample path. NO discard, NO alpha test. The
// mesher-enforced no-alpha-dependence invariant from block_opaque_vk.frag
// holds unchanged. Keep in step with the GL terrain_opaque.frag.
#version 450

// Input from vertex shader
layout (location = 0) in vec2 fragTexCoord;
layout (location = 1) in vec3 fragWorldPos;
layout (location = 2) in vec4 fragColor;
layout (location = 3) in vec4 fragTileRect;  // xy = sprite origin, zw = size; zw==0 = untiled

// Texture atlas sampler (descriptor set 0, binding 0)
layout (set = 0, binding = 0) uniform sampler2D uTextureAtlas;

// Push constants (must match C++ PushConstantBlock layout exactly)
layout (push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

// Common UBO (portal pipeline layout, set=1) — see block_vk.frag.
layout (std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;
    vec4  uPortalColor_;
    vec4  uColorDark_;
    vec4  uColorHot_;
    vec4  uKeyDir_;
    vec4  uTint_;
    vec4  uUVRange_;
    vec4  uScalarsA_;
    vec4  uScalarsB_;
    vec4  uScalarsC_;
    vec4  uScalarsD_;
    vec2  uScreenSize_;
    vec2  _pad_;
    vec4  uFogColor_;
    vec4  uFogEnv_;
    vec4  uCamPosBright_;
    vec4  uOverlayColor_;  // debug fill override (a=0 normally)
} U;

// Output
layout (location = 0) out vec4 FragColor;

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
    finalColor *= U.uCamPosBright_.w;
    vec3 fogDelta = fragWorldPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    finalColor = mix(finalColor, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);

    finalColor = mix(finalColor, U.uOverlayColor_.rgb, U.uOverlayColor_.a);
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
