// File: shaders/lines.vert (OpenGL twin of lines_vk.vert) — Gizmos' screen-space
// lines: each vertex carries its line's direction octahedron-encoded in the uv
// and is pushed sideways by uLineWidth pixels, alternating sides per vertex.
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
uniform vec2 uScreenSize;
uniform float uLineWidth;
out vec4 vColor;
vec3 decodeOctahedron(vec2 e) {
    vec3 n = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
void main() {
    vec3 dir = decodeOctahedron(aUV);
    vec4 linePosStart = uMVP * vec4(aPos, 1.0);
    vec4 linePosEnd   = uMVP * vec4(aPos + dir, 1.0);
    vec3 ndc1 = linePosStart.xyz / linePosStart.w;
    vec3 ndc2 = linePosEnd.xyz / linePosEnd.w;
    vec2 lineScreenDirection = normalize((ndc2.xy - ndc1.xy) * uScreenSize);
    vec2 lineOffset = vec2(-lineScreenDirection.y, lineScreenDirection.x) * uLineWidth / uScreenSize;
    if (lineOffset.x < 0.0) lineOffset *= -1.0;
    if (gl_VertexID % 2 == 0) gl_Position = vec4((ndc1 + vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    else                      gl_Position = vec4((ndc1 - vec3(lineOffset, 0.0)) * linePosStart.w, linePosStart.w);
    vColor = aColor;
}
