// File: shaders/lines_vk.vert — MC rendertype_lines.vsh on this engine's block
// vertex layout: pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2).
// The line's unit direction (MC's Normal attribute) rides in UV, octahedron-
// encoded, since the layout has no third vec3. Each line is four vertices,
// two per endpoint; gl_VertexIndex parity picks the side of the strip.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // Proj * VIEW_SCALE * layering * ModelView, baked on the CPU
    vec2 uScreenSize;   // framebuffer size
    float uLineWidth;   // framebuffer pixels
    float uAlphaTest;
} pc;

layout(location = 0) out vec4 fragColor;

vec3 decodeOctahedron(vec2 e) {
    vec3 n = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void main() {
    vec3 dir = decodeOctahedron(aUV);
    vec4 linePosStart = pc.uMVP * vec4(aPos, 1.0);
    vec4 linePosEnd   = pc.uMVP * vec4(aPos + dir, 1.0);

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
