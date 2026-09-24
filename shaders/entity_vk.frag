// File: shaders/entity_vk.frag (Vulkan version of the mob entity shader)
// MC entity.fsh order: texture × vertex colour, overlay, light, fog — see
// entity.frag.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vRenderPos;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uColor;        // hurt flash / creeper swell overlay: rgb + strength
    vec4 uUVRange;      // the clip plane (vertex stage)
    vec4 uScalars;      // xyz: the batch's lightmap colour (EntityEnvironment.hpp)
} pc;

// Common UBO (portal pipeline layout, set = 1) — the frame's fog, as the
// terrain shaders read it. Prefix fields declared for the std140 offsets.
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
    vec4  uFogColor_;      // rgb = fog colour, a = strength
    vec4  uFogEnv_;        // (envStart, envEnd, rdStart, rdEnd)
    vec4  uCamPosBright_;  // xyz = camera (render space)
} U;

layout(location = 0) out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 t = texture(uTex, vUV);

    // Entity textures are cutout: the transparent regions of a 64x32 sheet must
    // be discarded, or every mob renders inside a black box.
    if (t.a < 0.05) discard;

    vec3 base = t.rgb * vColor.rgb;
    vec3 color = mix(base, pc.uColor.rgb, pc.uColor.a) * pc.uScalars.xyz;

    vec3 fogDelta = vRenderPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    color = mix(color, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);

    // Alpha carries through from the vertex colour — see entity.frag.
    FragColor = vec4(color, t.a * vColor.a);
}
