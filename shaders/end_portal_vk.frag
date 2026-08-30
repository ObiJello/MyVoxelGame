// File: shaders/end_portal_vk.frag
//
// Vulkan twin of shaders/end_portal.frag. The layer math, COLORS table and
// matrices are byte-identical to the GL version (and to Mojang's vendored
// assets/shaders/core/rendertype_end_portal.fsh) — only the uniform plumbing
// differs. Keep the three files in step.
#version 450

// MC RenderPipelines.java:211 — 15 for end_portal, 16 for end_gateway.
#define PORTAL_LAYERS 15

// Two samplers via the portal pipeline layout: VKBackend binds the texture at
// BindTexture slot 0 to set=0 and the one at slot 1 to set=2
// (BindPortalDescriptorForDraw). set=0 is Sampler0 (end_sky), set=2 is
// Sampler1 (end_portal).
layout(set = 0, binding = 0) uniform sampler2D uSky;
layout(set = 2, binding = 0) uniform sampler2D uPortal;

// Common UBO (portal pipeline layout, set=1). std140 offsets are positional,
// so the full prefix has to be declared even though only the last three
// vec4s and uScalarsA are read. uScalarsA_.x is what
// SetUniformFloat(shader, "uTime", …) writes.
layout(std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;
    vec4  uPortalColor_;
    vec4  uColorDark_;
    vec4  uColorHot_;
    vec4  uKeyDir_;
    vec4  uTint_;
    vec4  uUVRange_;
    vec4  uScalarsA_;      // 224 — x = uTime (MC's GameTime)
    vec4  uScalarsB_;
    vec4  uScalarsC_;
    vec4  uScalarsD_;
    vec2  uScreenSize_;
    vec2  _pad_;
    vec4  uFogColor_;      // 304 — rgb = fog color, a = fog color alpha
    vec4  uFogEnv_;        // 320 — (envStart, envEnd, rdStart, rdEnd)
    vec4  uCamPosBright_;  // 336 — xyz = uCameraPos, w = uSkyBrightness
} U;

layout(location = 0) in vec4 vTexProj;
layout(location = 1) in vec3 vWorldPos;

layout(location = 0) out vec4 FragColor;

const vec3 COLORS[16] = vec3[16](
    vec3(0.022087, 0.098399, 0.110818),
    vec3(0.011892, 0.095924, 0.089485),
    vec3(0.027636, 0.101689, 0.100326),
    vec3(0.046564, 0.109883, 0.114838),
    vec3(0.064901, 0.117696, 0.097189),
    vec3(0.063761, 0.086895, 0.123646),
    vec3(0.084817, 0.111994, 0.166380),
    vec3(0.097489, 0.154120, 0.091064),
    vec3(0.106152, 0.131144, 0.195191),
    vec3(0.097721, 0.110188, 0.187229),
    vec3(0.133516, 0.138278, 0.148582),
    vec3(0.070006, 0.243332, 0.235792),
    vec3(0.196766, 0.142899, 0.214696),
    vec3(0.047281, 0.315338, 0.321970),
    vec3(0.204675, 0.390010, 0.302066),
    vec3(0.080955, 0.314821, 0.661491)
);

// Reads row-major and is applied row-major — see the note in the GL twin.
const mat4 SCALE_TRANSLATE = mat4(
    0.5, 0.0, 0.0, 0.25,
    0.0, 0.5, 0.0, 0.25,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0
);

mat2 mat2_rotate_z(float radians) {
    return mat2(
        cos(radians), -sin(radians),
        sin(radians), cos(radians)
    );
}

mat4 end_portal_layer(float layer) {
    mat4 translate = mat4(
        1.0, 0.0, 0.0, 17.0 / layer,
        0.0, 1.0, 0.0, (2.0 + layer / 1.5) * (U.uScalarsA_.x * 1.5),
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    );

    mat2 rotate = mat2_rotate_z(radians((layer * layer * 4321.0 + layer * 9.0) * 2.0));

    mat2 scale = mat2((4.5 - layer / 4.0) * 2.0);

    return mat4(scale * rotate) * translate * SCALE_TRANSLATE;
}

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec3 color = textureProj(uSky, vTexProj).rgb * COLORS[0];
    for (int i = 0; i < PORTAL_LAYERS; i++) {
        color += textureProj(uPortal, vTexProj * end_portal_layer(float(i + 1))).rgb * COLORS[i];
    }

    vec3 fogDelta = vWorldPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    color = mix(color, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);

    FragColor = vec4(color, 1.0);
}
