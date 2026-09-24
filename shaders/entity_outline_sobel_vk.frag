// File: shaders/entity_outline_sobel_vk.frag (Vulkan twin of
// entity_outline_sobel.frag — MC post/entity_sobel.fsh).
#version 450

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uInSize;       // 64 — the uScreenSize slot (VKBackend routes uInSize)
} pc;

void main() {
    vec2 oneTexel = 1.0 / pc.uInSize;

    vec4 center = texture(uTex, texCoord);
    vec4 left = texture(uTex, texCoord - vec2(oneTexel.x, 0.0));
    vec4 right = texture(uTex, texCoord + vec2(oneTexel.x, 0.0));
    vec4 up = texture(uTex, texCoord - vec2(0.0, oneTexel.y));
    vec4 down = texture(uTex, texCoord + vec2(0.0, oneTexel.y));
    float leftDiff  = abs(center.a - left.a);
    float rightDiff = abs(center.a - right.a);
    float upDiff    = abs(center.a - up.a);
    float downDiff  = abs(center.a - down.a);
    float total = clamp(leftDiff + rightDiff + upDiff + downDiff, 0.0, 1.0);
    vec3 outColor = center.rgb * center.a + left.rgb * left.a + right.rgb * right.a + up.rgb * up.a + down.rgb * down.a;
    fragColor = vec4(outColor * 0.2, total);
}
