// File: shaders/highlight_vk.vert (Vulkan version of block highlight GL shader)
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2)
// UV.x encodes axis-aligned edge direction: 0=X, 1=Y, 2=Z
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

layout(location = 0) out vec4 fragColor;

const float VIEW_SHRINK = 1.0 - (1.0 / 256.0);
const mat4 VIEW_SCALE = mat4(
    VIEW_SHRINK, 0.0, 0.0, 0.0,
    0.0, VIEW_SHRINK, 0.0, 0.0,
    0.0, 0.0, VIEW_SHRINK, 0.0,
    0.0, 0.0, 0.0, 1.0
);

void main() {
    // Decode edge direction from UV.x: 0=X, 1=Y, 2=Z
    int axis = int(aUV.x + 0.5);
    vec3 edgeDir = vec3(0.0);
    if (axis == 0) edgeDir.x = 1.0;
    else if (axis == 1) edgeDir.y = 1.0;
    else edgeDir.z = 1.0;

    // VIEW_SCALE is pre-baked into uMVP on CPU: Proj * VIEW_SCALE * ModelView
    vec4 linePosStart = pc.uMVP * vec4(aPos, 1.0);
    vec4 linePosEnd   = pc.uMVP * vec4(aPos + edgeDir, 1.0);

    vec3 ndc1 = linePosStart.xyz / linePosStart.w;
    vec3 ndc2 = linePosEnd.xyz / linePosEnd.w;

    vec2 lineScreenDirection = normalize((ndc2.xy - ndc1.xy) * pc.uScreenSize);
    vec2 lineOffset = vec2(-lineScreenDirection.y, lineScreenDirection.x) * pc.uLineWidth / pc.uScreenSize;

    if (lineOffset.x < 0.0) {
        lineOffset *= -1.0;
    }

    if (gl_VertexIndex % 2 == 0) {
        gl_Position = vec4((ndc1 + vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    } else {
        gl_Position = vec4((ndc1 - vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    }

    fragColor = aColor;
}
