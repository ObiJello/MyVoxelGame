// File: shaders/viewmodel_skinned_vk.vert
// Vulkan skinned viewmodel vertex shader. Uses CommonUBO (set=0 binding=1)
// for uMVP/uModel/uUseSkin and BonesUBO (set=0 binding=2) for the 96-bone
// palette.
#version 450

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec2  aUV;
layout(location = 2) in vec3  aNormal;
// UINT format on Vulkan side — uvec4 declaration here is REQUIRED for
// VK_FORMAT_R8G8B8A8_UINT (Vulkan rejects vec4 against an integer
// vertex format). int(aJoints.x) below converts back to the int the
// skinning math wants.
layout(location = 3) in uvec4 aJoints;
layout(location = 4) in vec4  aWeights;

layout(std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;            //   0
    mat4  uModel_;          //  64
    vec4  uPortalColor_;    // 128
    vec4  uColorDark_;      // 144
    vec4  uColorHot_;       // 160
    vec4  uKeyDir_;         // 176 — xyz=dir,  w=uKeyIntensity
    vec4  uTint_;           // 192
    vec4  uUVRange_;        // 208
    vec4  uScalarsA_;       // 224
    vec4  uScalarsB_;       // 240
    vec4  uScalarsC_;       // 256 — (uAmbient, uAlphaCutoff, uExposure, uHasBloom)
    vec4  uScalarsD_;       // 272 — (uHasSprite, uUseSkin, uUseTextures, _pad)
    vec2  uScreenSize_;     // 288
    vec2  _pad_;            // 296
} U;
layout(std140, set = 1, binding = 1) uniform Bones {
    mat4 uBones_[96];
} B;

#define uMVP        U.uMVP_
#define uModel      U.uModel_
#define uUseSkin    U.uScalarsD_.y
#define uBones      B.uBones_

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vNormalCam;

void main() {
    vec4 skinned;
    vec3 nrm;
    if (uUseSkin > 0.5) {
        int i0 = int(aJoints.x);
        int i1 = int(aJoints.y);
        int i2 = int(aJoints.z);
        int i3 = int(aJoints.w);
        float wSum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
        vec4  w    = aWeights / max(wSum, 1e-4);
        mat4 skin = w.x * uBones[i0]
                  + w.y * uBones[i1]
                  + w.z * uBones[i2]
                  + w.w * uBones[i3];
        skinned = skin * vec4(aPos, 1.0);
        nrm     = normalize(mat3(skin) * aNormal);
    } else {
        skinned = vec4(aPos, 1.0);
        nrm     = aNormal;
    }
    gl_Position = uMVP * skinned;
    vUV         = aUV;
    vNormalCam  = mat3(uModel) * nrm;
}
