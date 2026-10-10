// File: shaders/clouds.frag (OpenGL twin of clouds_vk.frag) — CloudRenderer.
// The Improved Transparency variants are this source with OIT and a stage
// define; the GL backend splices shaders/oit_lib.glsl in at the pragma.
#version 330 core
in vec4 vColor;
in float vDist;
#ifndef OIT_ALPHA_ONLY
out vec4 FragColor;
#endif

uniform vec4 uColor;     // CLOUD_COLOR attribute (rgba, alpha carries 0.8 base)
uniform vec4 uFogEnv;    // (0, cloudsEnd, unused, unused)

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

#ifdef OIT
#pragma oit_library
#endif

void main() {
    vec4 color = vColor * uColor;
#ifndef OIT_DEPTH_BOUNDS
    color.a *= 1.0 - linearFog(vDist, uFogEnv.x, uFogEnv.y);   // (MC: not in the depth bounds)
#endif
    if (color.a <= 0.0) discard;
#ifdef OIT_ALPHA_ONLY
    executeAlphaOnlyPhase(gl_FragCoord.z, color.a);   // MC clouds.fsh
#else
    FragColor = color;
#ifdef OIT_ACCUMULATE
    FragColor = sampleColorForAccumulation(FragColor);   // MC calculateFinalColor
#endif
#endif
}
