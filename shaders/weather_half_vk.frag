// File: shaders/weather_half_vk.frag
// Rain and snow at half the world's resolution (WeatherEffectRenderer,
// Video Settings → Rain Resolution: Half). MC particle.fsh as in
// mob_particle_vk.frag — texture × colour, the 0.1 cutout, the draw's light,
// the fog — drawn into a target half the world's size, so two additions:
//  - the world's depth test, here instead of in a depth attachment, against
//    the PREVIOUS frame's depth: the pass runs before this frame's world is
//    drawn (ahead of the frame's render pass, so the pass never breaks).
//    The fragment is reprojected to where it was in that frame (U.uModel_
//    = that frame's view-projection, GL depth convention, in this frame's
//    render space), and dropped behind the FARTHEST of the four depth texels
//    it covers there (a streak may overlap a silhouette by up to one
//    full-resolution pixel rather than leave a gap). The terrain is static,
//    so the test is exact under any camera motion; where that frame has no
//    depth (off its screen, behind its camera) the fragment is drawn. Stored
//    depth below pc.uAlphaTest is the hand, the portal gun or a GUI item
//    (their bands near 0, LateDepthBands.hpp), not an occluder. Tested
//    after the alpha cutout, which ~97 % of rain fragments fail first.
//  - premultiplied output: the layers accumulate as "over" with the blend
//    One / OneMinusSrcAlpha, and the composite lays the result over the
//    frame with the same blend — what the full-resolution pass's
//    SrcAlpha / OneMinusSrcAlpha did layer by layer.
#version 450

layout(set = 0, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform PC {
    mat4  uMVP;
    vec2  uScreenSize;
    float uLineWidth;
    float uAlphaTest;    // here: the depth below which nothing is the world's
    vec4  uColor;
    vec4  uUVRange;
    vec4  uScalars;      // xyz: the draw's lightmap colour (EntityEnvironment.hpp)
} pc;

// Common UBO (portal pipeline layout, set = 1) — see entity_vk.frag.
layout (std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;         // here: the previous frame's view-projection
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
    vec4  uFogColor_;      // rgb = fog colour, a = strength
    vec4  uFogEnv_;        // (envStart, envEnd, rdStart, rdEnd)
    vec4  uCamPosBright_;  // xyz = camera (render space)
} U;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vRenderPos;
layout(location = 0) out vec4 FragColor;

// The previous frame's depth (RenderBackend::PreviousFrameDepthTexture),
// texture slot 1.
layout(set = 2, binding = 0) uniform sampler2D uSceneDepth;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

// One texel of the previous frame's depth; far where there is none or
// where it holds a band rather than the world.
float previousDepth(ivec2 texel, ivec2 size) {
    if (any(lessThan(texel, ivec2(0))) || any(greaterThanEqual(texel, size))) return 1.0;
    float d = texelFetch(uSceneDepth, texel, 0).r;
    return d < pc.uAlphaTest ? 1.0 : d;
}

void main() {
    vec4 c = texture(uSprite, vUV) * vColor;
    if (c.a < 0.1) discard;
    vec4 clip = U.uModel_ * vec4(vRenderPos, 1.0);
    if (clip.w > 0.0) {
        vec3 ndc = clip.xyz / clip.w;
        ivec2 size = textureSize(uSceneDepth, 0);
        // The frame's flipped viewport: NDC +y is the image's top row.
        vec2 pixel = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * vec2(size);
        ivec2 base = ivec2(floor(pixel - 0.5));
        float scene = max(max(previousDepth(base, size), previousDepth(base + ivec2(1, 0), size)),
                          max(previousDepth(base + ivec2(0, 1), size), previousDepth(base + ivec2(1, 1), size)));
        if (ndc.z * 0.5 + 0.5 > scene) discard;   // the full-resolution pass's LessEqual
    }
    c.rgb *= pc.uScalars.xyz;
    vec3 fogDelta = vRenderPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    c.rgb = mix(c.rgb, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);
    FragColor = vec4(c.rgb * c.a, c.a);
}
