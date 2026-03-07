#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
} pc;

layout(location = 0) out vec4 fragColor;

void main() {
    // VIEW_SCALE is pre-baked into uMVP on CPU: Proj * VIEW_SCALE * ModelView
    vec4 linePosStart = pc.uMVP * vec4(aPos, 1.0);
    vec4 linePosEnd   = pc.uMVP * vec4(aPos + aNormal, 1.0);

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
