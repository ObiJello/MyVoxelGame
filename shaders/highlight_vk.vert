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
    vec4 clipPos  = pc.uMVP * vec4(aPos, 1.0);
    vec4 clipPos2 = pc.uMVP * vec4(aPos + aNormal, 1.0);

    vec2 ndcPos  = clipPos.xy  / clipPos.w;
    vec2 ndcPos2 = clipPos2.xy / clipPos2.w;

    vec2 screenDir = (ndcPos2 - ndcPos) * pc.uScreenSize;
    vec2 perp = normalize(vec2(-screenDir.y, screenDir.x));

    float side = (gl_VertexIndex % 2 == 0) ? -1.0 : 1.0;
    vec2 offset = perp * pc.uLineWidth * side / pc.uScreenSize;

    gl_Position = clipPos + vec4(offset * clipPos.w, 0.0, 0.0);
    fragColor = aColor;
}
