#version 330 core

// Mob entity fragment shader (OpenGL). MC entity.fsh, in its order:
// texture × vertex colour, the overlay (hurt flash / creeper whiteout), the
// light, then the fog — the terrain's own fog, so a mob fades into the
// Blindness black exactly where the ground under it does.

in vec2 vUV;
in vec4 vColor;
in vec3 vRenderPos;
out vec4 FragColor;

uniform sampler2D uTex;

// Hurt flash and the creeper's pre-detonation whiteout, in one uniform:
//   rgb = the colour to blend toward
//   a   = how much of it (0 = untouched)
//
// Named uColor, not uOverlay: VKBackend::SetUniformVec4 routes vec4 uniforms by
// NAME into a fixed push-constant slot, and only uTint/uColor/uClipPlane/
// uPortalClipPlane are recognised. A shader here that declared uOverlay would
// work on OpenGL and silently render unlit on Vulkan.
uniform vec4 uColor;

// The lightmap stand-in for this batch (EntityEnvironment.hpp): the sky dim for
// an ordinary mob, full block light for a burning / fullbright one, 1 for an
// emissive layer (eyes, the warden's glow). Always set by the renderer.
uniform vec3 uEntityLight;    // the draw's lightmap colour (EntityEnvironment.hpp)
// The frame's fog, packed as the terrain shaders take it.
uniform vec3 uCameraPos;   // render space
uniform vec4 uFogColor;    // rgb, a = strength
uniform vec4 uFogEnv;      // (envStart, envEnd, rdStart, rdEnd)

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 t = texture(uTex, vUV);

    // Entity textures are cutout, not blended: the transparent regions of a
    // 64x32 sheet must be discarded, or every mob renders inside a black box.
    if (t.a < 0.05) discard;

    vec3 base = t.rgb * vColor.rgb;
    vec3 color = mix(base, uColor.rgb, uColor.a) * uEntityLight;

    vec3 fogDelta = vRenderPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    color = mix(color, uFogColor.rgb, fogValue * uFogColor.a);

    // Alpha carries through from the vertex colour: the warden's pulsating
    // emissive layers (MC RenderTypes.entityTranslucentEmissive) fade by a
    // per-batch vertex alpha with blending enabled. Every ordinary batch
    // draws with blending OFF and vertex alpha 1, so this changes nothing
    // for them.
    FragColor = vec4(color, t.a * vColor.a);
}
