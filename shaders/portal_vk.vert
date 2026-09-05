// File: shaders/portal_vk.vert
// Vulkan version of the portal renderer's vertex shader. Uses the
// portal pipeline layout's CommonUBO (set=0, binding=1) for uniforms.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;            //   0
    mat4  uModel_;          //  64
    vec4  uPortalColor_;    // 128 — rgb=color, w=uPulse
    vec4  uColorDark_;      // 144 — rgb=dark,  w=uOpenAmount
    vec4  uColorHot_;       // 160 — rgb=hot,   w=uOpenAmountVS
    vec4  uKeyDir_;         // 176
    vec4  uTint_;           // 192
    vec4  uUVRange_;        // 208
    vec4  uScalarsA_;       // 224 — (uTime, uTimeVS, uStaticAmount, uColorScale)
    vec4  uScalarsB_;       // 240 — (uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity)
    vec4  uScalarsC_;       // 256
    vec4  uScalarsD_;       // 272 — (uHasSprite, uUseSkin, uUseTextures, _pad)
    vec2  uScreenSize_;     // 288
    vec2  _pad_;            // 296
} c;

// Aliases so the shader body reads like the GL version.
#define uMVP            c.uMVP_
#define uPulse          c.uPortalColor_.w
#define uTimeVS         c.uScalarsA_.y
#define uOpenAmountVS   c.uColorHot_.w
// The world-space clip plane rides the uTint slot (VKBackend::SetUniformVec4
// aliases uPortalClipPlane onto it); the portal shaders use no tint. See
// portal.vert for what it cuts.
#define uPortalClipPlane c.uTint_

// Explicit gl_PerVertex redeclaration so gl_ClipDistance[0] actually lands —
// see the long note in block_vk.vert.
out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vNoiseUV;
layout(location = 2) out vec3 vWorldPos;   // for the fog overlay (uOutlineMode 3)

void main() {
    vec3 pos = vec3(aPos.x * uPulse, aPos.y * uPulse, aPos.z);
    gl_Position = uMVP * vec4(pos, 1.0);
    vWorldPos = (c.uModel_ * vec4(pos, 1.0)).xyz;
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, vWorldPos) + uPortalClipPlane.w
        : 1.0;

    const float kOuterBorder = 0.075;
    vUV = aUV * (1.0 + kOuterBorder) - vec2(kOuterBorder * 0.5);

    float openAmtVS = clamp(uOpenAmountVS + 0.001, 0.0, 1.0);
    const float kNoiseScale  = 0.3;
    const float kScrollRate  = 0.0275;
    float scroll = uTimeVS * kScrollRate;
    vec2 noiseBase = (aUV - 0.5) / openAmtVS + 0.5;
    vNoiseUV.xy = noiseBase * kNoiseScale + vec2( scroll, 0.0);
    vNoiseUV.zw = noiseBase * kNoiseScale - vec2( scroll, 0.0);
}
