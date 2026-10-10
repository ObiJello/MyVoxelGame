// File: shaders/highlight.vert (OpenGL twin of highlight_vk.vert) — the block
// outline (BlockHighlight): MC's rendertype_lines, each edge's direction in
// uv.x (0 = X, 1 = Y, 2 = Z), widened to uLineWidth pixels on screen.
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uProjMat;
uniform mat4 uModelViewMat;
uniform vec2 uScreenSize;
uniform float uLineWidth;

out vec4 vColor;

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

    vec4 linePosStart = uProjMat * VIEW_SCALE * uModelViewMat * vec4(aPos, 1.0);
    vec4 linePosEnd   = uProjMat * VIEW_SCALE * uModelViewMat * vec4(aPos + edgeDir, 1.0);

    vec3 ndc1 = linePosStart.xyz / linePosStart.w;
    vec3 ndc2 = linePosEnd.xyz / linePosEnd.w;

    vec2 lineScreenDirection = normalize((ndc2.xy - ndc1.xy) * uScreenSize);
    vec2 lineOffset = vec2(-lineScreenDirection.y, lineScreenDirection.x) * uLineWidth / uScreenSize;

    if (lineOffset.x < 0.0) {
        lineOffset *= -1.0;
    }

    if (gl_VertexID % 2 == 0) {
        gl_Position = vec4((ndc1 + vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    } else {
        gl_Position = vec4((ndc1 - vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    }

    vColor = aColor;
}
