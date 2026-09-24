#version 330 core
// File: shaders/entity_outline_sobel.frag (OpenGL) — MC post/entity_sobel.fsh
// verbatim: the silhouette's edge, one texel either side of it, carrying the
// team colours around it.
in vec2 texCoord;
out vec4 fragColor;

uniform sampler2D uInSampler;   // MC InSampler: the silhouettes
uniform vec2 uInSize;     // MC SamplerInfo.InSize

void main() {
    vec2 oneTexel = 1.0 / uInSize;

    vec4 center = texture(uInSampler, texCoord);
    vec4 left = texture(uInSampler, texCoord - vec2(oneTexel.x, 0.0));
    vec4 right = texture(uInSampler, texCoord + vec2(oneTexel.x, 0.0));
    vec4 up = texture(uInSampler, texCoord - vec2(0.0, oneTexel.y));
    vec4 down = texture(uInSampler, texCoord + vec2(0.0, oneTexel.y));
    float leftDiff  = abs(center.a - left.a);
    float rightDiff = abs(center.a - right.a);
    float upDiff    = abs(center.a - up.a);
    float downDiff  = abs(center.a - down.a);
    float total = clamp(leftDiff + rightDiff + upDiff + downDiff, 0.0, 1.0);
    vec3 outColor = center.rgb * center.a + left.rgb * left.a + right.rgb * right.a + up.rgb * up.a + down.rgb * down.a;
    fragColor = vec4(outColor * 0.2, total);
}
