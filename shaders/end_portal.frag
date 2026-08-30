// File: shaders/end_portal.frag
//
// Mojang's core/rendertype_end_portal.fsh with its #moj_imports substituted
// and apply_fog replaced by this engine's fog. The layer math, the COLORS
// table and the two matrices are copied VERBATIM from the vendored original
// (assets/shaders/core/rendertype_end_portal.fsh) — the look is entirely a
// product of those magic numbers, so nothing here should be "tidied".
// shaders/end_portal_vk.frag is the same shader against the Vulkan bindings.
#version 330 core

// MC RenderPipelines.java:211 — the end_portal pipeline defines PORTAL_LAYERS
// as 15; end_gateway (line 212) reuses the same shader with 16. The COLORS
// table is shared between the two, which is why it has 16 entries and the
// last one is dead here.
#define PORTAL_LAYERS 15

uniform sampler2D uSky;      // MC Sampler0 — textures/environment/end_sky.png
uniform sampler2D uPortal;   // MC Sampler1 — textures/entity/end_portal.png

// MC's GameTime (assets/shaders/include/globals.glsl), fed as
// ((gameTime % 24000) + partialTick) / 24000 — GlobalSettingsUniform.java:30.
// Spelled "uTime" rather than "uGameTime" on purpose: that is the name
// VKBackend::SetUniformFloat routes into the CommonUBO scalar slot the Vulkan
// twin of this shader reads, and one name keeps both backends on one setter.
uniform float uTime;

// Engine fog, lifted from shaders/block_solid.frag, in place of Mojang's
// apply_fog. Deliberately NO uSkyBrightness multiply: MC's END_PORTAL pipeline
// has no lightmap sampler, so the portal is fully emissive and does not dim
// at night.
uniform vec3 uCameraPos;
uniform vec4 uFogColor;
uniform vec4 uFogEnv;   // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off

in vec4 vTexProj;
in vec3 vWorldPos;

out vec4 FragColor;

// Explicitly sized (the original relies on an implicitly sized const array,
// which some GLSL 3.30 drivers are fussy about). Values unchanged.
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

// These literals read row-major, and that is correct: GLSL fills a mat4 from
// scalars COLUMN-major, but the only use below is `vec4 * mat4`, which is
// row-vector multiplication (equivalent to transpose(M) * v). The two
// transposes cancel, so what you read is what is applied.
const mat4 SCALE_TRANSLATE = mat4(
    0.5, 0.0, 0.0, 0.25,
    0.0, 0.5, 0.0, 0.25,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0
);

// assets/shaders/include/matrix.glsl, verbatim.
mat2 mat2_rotate_z(float radians) {
    return mat2(
        cos(radians), -sin(radians),
        sin(radians), cos(radians)
    );
}

// One starfield layer's transform. Each layer scrolls at its own speed off
// uTime, is rotated by a per-layer constant angle and scaled down as the layer
// index rises — 15 of them stacked additively is the depth illusion.
mat4 end_portal_layer(float layer) {
    mat4 translate = mat4(
        1.0, 0.0, 0.0, 17.0 / layer,
        0.0, 1.0, 0.0, (2.0 + layer / 1.5) * (uTime * 1.5),
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

    // MC-style distance fog (shaders/block_solid.frag).
    vec3 fogDelta = vWorldPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    color = mix(color, uFogColor.rgb, fogValue * uFogColor.a);

    // Opaque: the END_PORTAL pipeline declares no blend function
    // (RenderPipelines.java:154/211), so alpha is a constant 1.
    FragColor = vec4(color, 1.0);
}
