#version 450
// File: shaders/oit_depth_bounds_cull_vk.frag (Vulkan twin of oit_depth_bounds_cull.frag) — MC 26.3
// core/oit_depth_bounds_cull.fsh. Drawn into the culled depth bounds with the
// OIT depth tested and WRITTEN: the far bound pulled in to the nearest fully
// opaque OIT surface, whose depth goes into the depth buffer so the later
// stages skip what it hides. Bounds hold 1 - depth (oit_lib.glsl).
#define OIT
#extension GL_GOOGLE_include_directive : require
#include "oit_lib.glsl"

layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 pixelCoords = ivec2(gl_FragCoord.xy);
    vec4 depthBounds = texelFetch(DepthBoundsSampler, pixelCoords, 0);

    float closestBoundLinearDepthNegated = depthBounds.r;
    float furthestBoundLinearDepth = depthBounds.g;
    float closestBoundNearness = depthBounds.b;
    float opaqueOitNearness = depthBounds.a;

    float opaqueOitDeviceDepth = 1.0 - opaqueOitNearness;
    float opaqueOitLinearDepth = deviceToLinearDepth(opaqueOitDeviceDepth);

    fragColor = vec4(closestBoundLinearDepthNegated, min(furthestBoundLinearDepth, opaqueOitLinearDepth),
                     closestBoundNearness, opaqueOitNearness);
    gl_FragDepth = opaqueOitDeviceDepth;
}
