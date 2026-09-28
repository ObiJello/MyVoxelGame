#version 450
// File: shaders/oit_composite_vk.frag (Vulkan twin of oit_composite.frag) — MC 26.3 core/oit_composite.fsh
// Improved Transparency's resolve (Render::ImprovedTransparency): the
// accumulated colour, renormalised to the total coverage the transmittance
// coefficients give, blended premultiplied over the frame (MC
// TRANSLUCENT_PREMULTIPLIED_ALPHA); the nearest translucent depth written
// (MC's gl_FragDepth, depth ALWAYS). The engine's depth is not reversed:
// the bounds hold 1 - depth (oit_lib.glsl).
#define OIT
#extension GL_GOOGLE_include_directive : require
#include "oit_lib.glsl"

layout(set = 0, binding = 0) uniform sampler2D Sampler0;   // the accumulation (slot 0)

layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 pixelCoords = ivec2(gl_FragCoord.xy);
    vec4 accumulatedColor = texelFetch(Sampler0, pixelCoords, 0);

    float sampledTransmittance = sampleTransmittance(pixelCoords, 100000.0, 1.0);
    float coverage = 1.0 - (sampledTransmittance < OIT_FULLY_OPAQUE_TOTAL_TRANSMITTANCE ? 0.0 : sampledTransmittance);

    // Additive surfaces contribute colour but no coverage, so they only show
    // up in rgb. Discard pixels that have neither coverage nor additive light.
    if (coverage < 0.00001 && dot(accumulatedColor.rgb, vec3(1.0)) < 0.00001) {
        discard;
    }

    // Additive-only pixels have accumulatedColor.a ~ 0 (no coverage): no
    // renormalisation, their premultiplied light passes through untouched.
    float normalization = accumulatedColor.a > 0.00001 ? coverage / accumulatedColor.a : 1.0;
    fragColor = vec4(accumulatedColor.rgb * normalization, coverage);

    float closestBoundNearness = texelFetch(DepthBoundsSampler, pixelCoords, 0).b;
    gl_FragDepth = 1.0 - closestBoundNearness;
}
