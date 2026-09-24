#version 330 core
// File: shaders/beam_volume.vert (OpenGL volumetric light beam — proxy)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;     // unused (block vertex layout)
layout(location = 2) in vec4 aColor;  // unused
uniform mat4 uMVP;   // view-projection, render space (no model: the proxy is built here)
// Per-draw parameters: seven vec4 "slots" (VolumetricBeam.cpp has the table).
// The names are the ones VKBackend routes into the Common UBO, so one C++ path
// feeds both backends.
uniform vec3  uPortalColor;  uniform float uPulse;
uniform vec3  uColorDark;    uniform float uOpenAmount;
uniform vec3  uColorHot;     uniform float uOpenAmountVS;
uniform vec3  uKeyDir;       uniform float uKeyIntensity;
uniform float uTime, uTimeVS, uStaticAmount, uColorScale;
uniform float uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity;
uniform vec4  uTint;
#define P0 vec4(uPortalColor, uPulse)
#define P1 vec4(uColorDark, uOpenAmount)
#define P2 vec4(uColorHot, uOpenAmountVS)
#define P3 vec4(uKeyDir, uKeyIntensity)
#define P4 vec4(uTime, uTimeVS, uStaticAmount, uColorScale)
#define P5 vec4(uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity)
#define P6 uTint
#define MVP uMVP
out vec3 vRenderPos;

// The proxy: a unit capped frustum (aPos = axial fraction, cos, sin) placed
// over the cone — its radius × 1.03 + 0.08 (circumscribing the 16-gon, room
// for the soft rim), from just behind the apex to the clip end (P6.w).
// Must match kProxyScale / kProxyPad in VolumetricBeam.cpp.
void main() {
    vec3 A = P0.xyz;
    vec3 D = P1.xyz;
    float L = P0.w;
    float sEnd = min(P6.w, L);
    vec3 helper = abs(D.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 U1 = normalize(cross(D, helper));
    vec3 W1 = cross(D, U1);
    float s = mix(-0.08, sEnd + 0.08, aPos.x);
    float r = (P1.w + (P2.w - P1.w) * clamp(s, 0.0, L) / L) * 1.03 + 0.08;
    vec3 pos = A + D * s + (U1 * aPos.y + W1 * aPos.z) * r;
    vRenderPos = pos;
    gl_Position = MVP * vec4(pos, 1.0);
}
