// File: shaders/stick_figure_vk.vert (Vulkan twin of stick_figure.vert).
// Created through CreateShaderFromFilesPortal: the fragment shader reads the
// frame's fog from the Common UBO; uMVP and the clip plane stay push
// constants.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

// Must match C++ PushConstantBlock. The uColor slot (80) carries uClipPlane
// (VKBackend routes the name there), uScalars.x (112) the body's light.
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          //  0
    vec2 uScreenSize;   // 64
    float uLineWidth;   // 72
    float uAlphaTest;   // 76
    vec4 uClipPlane;    // 80
    vec4 uUVRange;      // 96
    vec4 uScalars;      // 112
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vRenderPos;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vColor      = aColor;
    vRenderPos  = aPos;
}
