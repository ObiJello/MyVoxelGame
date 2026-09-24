#version 330 core
// File: shaders/blockentity.frag (OpenGL block-entity shader)
// MC's block-entity render types (entity.fsh): texture × vertex colour, the
// lightmap, the fog. The light is the draw's (uBlockEntityLight — the
// lightmap colour for the block entity's own cell, or block light 15 for a
// sign's glowing text or a lit campfire's food), the fog the terrain's, so a chest fades into the
// Blindness black exactly where the floor under it does.
in vec2 vUV;
in vec4 vColor;
in vec3 vRenderPos;
out vec4 FragColor;

uniform sampler2D uTex;
// Cutout threshold per renderer (chest / shulker / bed / sign 0.05,
// skull 0.1, campfire food 0.5).
uniform float uAlphaTest;
uniform vec3 uBlockEntityLight;
uniform vec3  uCameraPos;   // render space
uniform vec4  uFogColor;    // rgb, a = strength
uniform vec4  uFogEnv;      // (envStart, envEnd, rdStart, rdEnd)

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 t = texture(uTex, vUV);
    if (t.a < uAlphaTest) discard;
    vec4 c = t * vColor;
    c.rgb *= uBlockEntityLight;
    vec3 fogDelta = vRenderPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    c.rgb = mix(c.rgb, uFogColor.rgb, fogValue * uFogColor.a);
    FragColor = c;
}
