// File: shaders/sky.frag (OpenGL twin of sky_vk.frag) — see sky.vert.
#version 330 core
in vec2 vUV;
in vec4 vColor;
in float vSph;
in float vCyl;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec4 uColor;
uniform vec4 uFogColor;
uniform vec4 uFogEnv;   // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 color = texture(uTexture, vUV) * vColor * uColor;
    if (color.a == 0.0) discard;   // MC position_tex.fsh texel discard
    float fogValue = max(linearFog(vSph, uFogEnv.x, uFogEnv.y),
                         linearFog(vCyl, uFogEnv.z, uFogEnv.w));
    FragColor = vec4(mix(color.rgb, uFogColor.rgb, fogValue * uFogColor.a), color.a);
}
