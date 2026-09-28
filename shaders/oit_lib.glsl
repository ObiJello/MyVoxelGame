// File: shaders/oit_lib.glsl — MC 26.3 Improved Transparency (wavelet OIT)
//
// MC's shaders/include/oit.glsl with everything it includes (oit_common,
// oit_depth_sample, oit_depth_bounds, oit_add_transmittance, oit_sample) in
// one file, compiled only into Render::ImprovedTransparency's shaders:
//   - the OIT variants of the participating shaders — the shader's own
//     source with `#define OIT` and one stage define (OIT_DEPTH_BOUNDS /
//     OIT_TRANSMITTANCE with OIT_ALPHA_ONLY, or OIT_ACCUMULATE). OpenGL splices
//     this file in at the shader's `#pragma oit_library`; the Vulkan twins
//     `#include` it and are compiled once per stage with glslc -D (CMake);
//   - the composite and cull passes (oit_composite, oit_depth_bounds_cull).
// Nothing outside an `#ifdef OIT` block ever sees it.
//
// Two changes from MC, both forced by this engine's conventions:
//   - Depth is not reversed here (clear 1, LEQUAL). MC keeps device depths in
//     the depth-bounds target's b/a channels and MAX-blends them so the
//     largest — under reversed Z the nearest — wins. This stores 1 - depth
//     ("nearness") instead: MAX still finds the nearest, 0 still means "none",
//     and whoever reads a device depth back takes 1 - it.
//   - The projection terms deviceToLinearDepth needs (ProjMat[2][2],
//     ProjMat[3][2]) come in OitProjParams.xy. Both backends use GL clip
//     conventions (the Vulkan backend remaps clip z itself, VKBackend
//     kVkZCorrect), so NDC z = 2 * depth - 1 on both.

#define OIT_WAVELET_RANK 2
#define OIT_COEFF_COUNT 8
#define OIT_COEFF_ATTACHMENT_COUNT 2

#ifdef VULKAN
layout(set = 6, binding = 0) uniform sampler2D DepthBoundsSampler;
layout(set = 6, binding = 1) uniform sampler2D Coeff0;
layout(set = 6, binding = 2) uniform sampler2D Coeff1;
layout(std140, set = 6, binding = 3) uniform OitParams { vec4 OitProjParams; };
#else
uniform sampler2D DepthBoundsSampler;
uniform sampler2D Coeff0;
uniform sampler2D Coeff1;
uniform vec4 OitProjParams;   // x = ProjMat[2][2], y = ProjMat[3][2]
#endif

// ── oit_common.glsl ─────────────────────────────────────────────────────────
const float OIT_FULLY_OPAQUE_ALPHA = 0.99;
const float OIT_FULLY_OPAQUE_TOTAL_TRANSMITTANCE = 0.02;
// The number of depth bins is the number of coefficients: one scales, the
// rest form the wavelet tree over the depth range.
const int OIT_NUMBER_OF_DEPTH_BINS = OIT_COEFF_COUNT;

float deviceToLinearDepth(float deviceDepth) {
    float ndc = (deviceDepth - 0.5) * 2.0;
    return OitProjParams.y / (ndc + OitProjParams.x);
}

float toAbsorbance(float transmittance) {
    return clamp(-log(max(transmittance, 0.0001)), 0.0, 4.0);
}

float toTransmittance(float absorbance) {
    return clamp(exp(-absorbance), 0.0001, 1.0);
}

// ── oit_depth_sample.glsl ───────────────────────────────────────────────────
#if defined(OIT_TRANSMITTANCE) || defined(OIT_ACCUMULATE)
const float OIT_HIGH_PRECISION_DEPTH_THRESHOLD = 10.0;
const int OIT_LOW_PRECISION_DEPTH_BINS = 2;

float normalizeDepth(float fragmentDeviceDepth) {
    vec4 depthBoundsSample = texelFetch(DepthBoundsSampler, ivec2(gl_FragCoord.xy), 0);
    float closestBoundLinearDepthNegated = depthBoundsSample.r;
    float closestBoundLinearDepth = -closestBoundLinearDepthNegated;
    float furthestBoundLinearDepth = depthBoundsSample.g;

    float fragmentLinearDepth = deviceToLinearDepth(fragmentDeviceDepth);
    float range = max(furthestBoundLinearDepth - closestBoundLinearDepth, 0.0001);
    float depthWithinBounds = clamp(fragmentLinearDepth - closestBoundLinearDepth, 0.0, range);

    const float depthFractionWithHighPrecision =
        float(OIT_NUMBER_OF_DEPTH_BINS - OIT_LOW_PRECISION_DEPTH_BINS) / float(OIT_NUMBER_OF_DEPTH_BINS);
    float depthWithHighPrecision = depthFractionWithHighPrecision * range;

    if (depthWithHighPrecision <= OIT_HIGH_PRECISION_DEPTH_THRESHOLD) {
        return depthWithinBounds / range;
    } else if (depthWithinBounds <= OIT_HIGH_PRECISION_DEPTH_THRESHOLD) {
        return (depthWithinBounds / OIT_HIGH_PRECISION_DEPTH_THRESHOLD) * depthFractionWithHighPrecision;
    } else {
        return depthFractionWithHighPrecision +
               ((depthWithinBounds - OIT_HIGH_PRECISION_DEPTH_THRESHOLD) /
                (range - OIT_HIGH_PRECISION_DEPTH_THRESHOLD)) * (1.0 - depthFractionWithHighPrecision);
    }
}
#endif

#if defined(OIT_DEPTH_BOUNDS)
// ── oit_depth_bounds.glsl ───────────────────────────────────────────────────
layout(location = 0) out vec4 oitDepthBoundsOut;

void calculateDepthBounds(float fragmentDeviceDepth, float alpha) {
    float fragmentLinearDepth = deviceToLinearDepth(fragmentDeviceDepth);
    float nearness = 1.0 - fragmentDeviceDepth;
    float opaqueNearness = alpha > OIT_FULLY_OPAQUE_ALPHA ? nearness : 0.0;
    oitDepthBoundsOut = vec4(-fragmentLinearDepth, fragmentLinearDepth, nearness, opaqueNearness);
}

#elif defined(OIT_TRANSMITTANCE)
// ── oit_add_transmittance.glsl ──────────────────────────────────────────────
layout(location = 0) out vec4 oitCoeffOut[OIT_COEFF_ATTACHMENT_COUNT];

void addTransmittance(float transmittance) {
    float coefficients[OIT_COEFF_COUNT];
    for (int i = 0; i < OIT_COEFF_COUNT; i++) {
        coefficients[i] = 0.0;
    }

    float absorbance = toAbsorbance(transmittance);

    float originalDepth = normalizeDepth(gl_FragCoord.z);
    float depth = originalDepth * float(OIT_NUMBER_OF_DEPTH_BINS - 1) / float(OIT_NUMBER_OF_DEPTH_BINS);

    float averageAbsorbance = absorbance * (1.0 - depth);
    coefficients[0] = averageAbsorbance;

    int depthBinIndex = int(floor(originalDepth * float(OIT_NUMBER_OF_DEPTH_BINS - 1)));
    // The tree starts at the coefficient 1.
    int treeIndex = depthBinIndex + OIT_COEFF_COUNT;

    for (int waveletLevel = 0; waveletLevel <= OIT_WAVELET_RANK; waveletLevel++) {
        int power = OIT_WAVELET_RANK - waveletLevel;
        int parentIndex = treeIndex >> 1;
        float indexAtParentLevel = float(parentIndex & ((1 << power) - 1));

        int isRightHalf = treeIndex & 1;
        int waveletSign = 1 - (isRightHalf << 1);
        float waveletWidth = exp2(-float(power));
        float waveletHeightScale = exp2(float(power) * 0.5);
        float depthOffsetRelativeToWavelet = depth - waveletWidth * indexAtParentLevel;
        float distanceFromWaveletEdge = float(isRightHalf) * waveletWidth + float(waveletSign) * depthOffsetRelativeToWavelet;
        float differenceCoefficient = distanceFromWaveletEdge * waveletHeightScale * absorbance;
        coefficients[parentIndex] = differenceCoefficient;

        treeIndex = parentIndex;
    }

    for (int attachmentIndex = 0; attachmentIndex < OIT_COEFF_ATTACHMENT_COUNT; attachmentIndex++) {
        oitCoeffOut[attachmentIndex] = vec4(coefficients[attachmentIndex * 4 + 0],
                                            coefficients[attachmentIndex * 4 + 1],
                                            coefficients[attachmentIndex * 4 + 2],
                                            coefficients[attachmentIndex * 4 + 3]);
    }
}

#else
// ── oit_sample.glsl ─────────────────────────────────────────────────────────
float sampleAbsorbance(float coefficients[OIT_COEFF_COUNT], float originalDepth, float currentAbsorbance) {
    float averageAbsorbance = coefficients[0];
    if (averageAbsorbance == 0.0) {
        return 0.0;
    }

    float depthMeasuredInBins = originalDepth * float(OIT_NUMBER_OF_DEPTH_BINS - 1);

    float depth = depthMeasuredInBins / float(OIT_NUMBER_OF_DEPTH_BINS);

    float currentAverageAbsorbanceContribution = currentAbsorbance * (1.0 - depth);
    averageAbsorbance -= currentAverageAbsorbanceContribution;

    int treeIndexB = clamp(int(floor(depthMeasuredInBins)), 0, OIT_NUMBER_OF_DEPTH_BINS - 1);
    bool shouldSampleA = treeIndexB >= 1;
    int treeIndexA = shouldSampleA ? (treeIndexB - 1) : treeIndexB;

    treeIndexB += OIT_COEFF_COUNT;
    treeIndexA += OIT_COEFF_COUNT;

    // B is the sample at the depth bin the fragment is in, A the one just
    // before it.
    float sampleB = averageAbsorbance;
    float sampleA = shouldSampleA ? averageAbsorbance : 0.0;

    for (int waveletLevel = 0; waveletLevel <= OIT_WAVELET_RANK; waveletLevel++) {
        int power = OIT_WAVELET_RANK - waveletLevel;
        float waveletWidth = exp2(-float(power));
        float waveletHeightScale = exp2(float(power) * 0.5);

        int parentIndexB = treeIndexB >> 1;
        int isRightHalfB = treeIndexB & 1;
        int waveletSignB = 1 - (isRightHalfB << 1);

        float indexAtParentLevel = float(parentIndexB & ((1 << power) - 1));
        float depthOffsetRelativeToWavelet = depth - waveletWidth * indexAtParentLevel;
        float currentDifferenceCoefficient =
            (float(isRightHalfB) * waveletWidth + float(waveletSignB) * depthOffsetRelativeToWavelet) *
            waveletHeightScale * currentAbsorbance;

        float differenceCoefficientB = coefficients[parentIndexB];
        differenceCoefficientB -= currentDifferenceCoefficient;

        sampleB -= waveletHeightScale * differenceCoefficientB * float(waveletSignB);
        treeIndexB = parentIndexB;

        if (shouldSampleA) {
            int parentIndexA = treeIndexA >> 1;
            float differenceCoefficientA = (parentIndexA == parentIndexB) ? differenceCoefficientB : coefficients[parentIndexA];
            int isRightHalfA = treeIndexA & 1;
            int waveletSignA = 1 - (isRightHalfA << 1);
            sampleA -= waveletHeightScale * differenceCoefficientA * float(waveletSignA);
            treeIndexA = parentIndexA;
        }
    }

    float lerpAlpha = depthMeasuredInBins >= float(OIT_NUMBER_OF_DEPTH_BINS) ? 1.0 : fract(depthMeasuredInBins);

    return mix(sampleA, sampleB, lerpAlpha);
}

float sampleTransmittance(ivec2 pos, float depth, float currentTransmittance) {
    float coefficients[OIT_COEFF_COUNT];
    vec4 c0 = texelFetch(Coeff0, pos, 0);
    vec4 c1 = texelFetch(Coeff1, pos, 0);
    coefficients[0] = c0.x; coefficients[1] = c0.y; coefficients[2] = c0.z; coefficients[3] = c0.w;
    coefficients[4] = c1.x; coefficients[5] = c1.y; coefficients[6] = c1.z; coefficients[7] = c1.w;
    return toTransmittance(sampleAbsorbance(coefficients, depth, toAbsorbance(currentTransmittance)));
}

#ifdef OIT_ACCUMULATE
vec4 sampleColorForAccumulation(vec4 color) {
    #ifdef OIT_ADDITIVE
    float transmittance = 1.0;
    float accumAlpha = 0.0;
    #else
    float transmittance = 1.0 - color.a;
    float accumAlpha = color.a;
    #endif
    float sampledTransmittance = sampleTransmittance(ivec2(gl_FragCoord.xy), normalizeDepth(gl_FragCoord.z), transmittance);
    return vec4(color.rgb * color.a, accumAlpha) * sampledTransmittance;
}
#endif
#endif

// ── oit.glsl ────────────────────────────────────────────────────────────────
void executeAlphaOnlyPhase(float deviceDepth, float alpha) {
    #ifdef OIT_DEPTH_BOUNDS
    if (alpha < 0.01) {
        discard;
    }
    calculateDepthBounds(deviceDepth, alpha);
    #elif defined(OIT_ADDITIVE)
    // Additive surfaces do not occlude: nothing for the transmittance.
    discard;
    #elif defined(OIT_TRANSMITTANCE)
    addTransmittance(1.0 - alpha);
    #endif
}
