// File: shaders/viewmodel.vert
// Vertex shader for the portal-gun viewmodel (and any future viewmodels).
// See PortalGunViewmodel.cpp for the matrix conventions — model is in
// metres, the view matrix is identity (we draw in camera space), and the
// projection is a narrow 54° viewmodel projection.

#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aNormal;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec2 vUV;
out vec3 vWorldNormal;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV         = aUV;
    // Model is in view space; we just need a stable normal for the
    // cheap rim/diffuse shading in the fragment shader. No need to
    // do the full transpose-inverse — uModel only contains rigid
    // transforms so the upper-3x3 already preserves normals.
    vWorldNormal = mat3(uModel) * aNormal;
}
