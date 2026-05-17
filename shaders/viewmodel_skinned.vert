// File: shaders/viewmodel_skinned.vert
// OpenGL skinned viewmodel vertex shader — mirrors kSkinnedVS in PortalGunViewmodel.cpp.
#version 330 core
layout(location = 0) in vec3  aPos;
layout(location = 1) in vec2  aUV;
layout(location = 2) in vec3  aNormal;
layout(location = 3) in vec4  aJoints;
layout(location = 4) in vec4  aWeights;

uniform mat4  uMVP;
uniform mat4  uModel;
uniform mat4  uBones[96];
uniform float uUseSkin;

out vec2 vUV;
out vec3 vNormalCam;

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
