// File: shaders/viewmodel_skinned.frag
// OpenGL viewmodel fragment shader — mirrors kSkinnedFS in PortalGunViewmodel.cpp.
#version 330 core
uniform sampler2D uDiffuse;
uniform float     uAlphaCutoff;
uniform vec3      uKeyDir;
uniform float     uKeyIntensity;
uniform float     uAmbient;
in vec2 vUV;
in vec3 vNormalCam;
out vec4 FragColor;
void main() {
    vec4 t = texture(uDiffuse, vUV);
    if (uAlphaCutoff > 0.0 && t.a < uAlphaCutoff) discard;
    vec3  n     = normalize(vNormalCam);
    float ndl   = max(dot(n, -normalize(uKeyDir)), 0.0);
    float halfL = pow(ndl * 0.5 + 0.5, 2.0);
    float shade = clamp(uAmbient + uKeyIntensity * halfL, 0.0, 1.5);
    FragColor   = vec4(t.rgb * shade, t.a);
}
