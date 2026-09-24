#version 330 core
// File: shaders/stick_figure.frag (OpenGL) — stick-figure players, lit and
// fogged like every other entity (EntityEnvironment.hpp).
in vec3 vRenderPos;
in vec4 vColor;
out vec4 FragColor;

// Optional clip plane for the portal half-body ghost, render space:
// xyz = normal, w = -dot(normal, point on plane); vec4(0) = off.
uniform vec4 uClipPlane;
// The body's light (the sky dim, or full block light while burning).
uniform vec3 uEntityLight;
uniform vec3  uCameraPos;   // render space
uniform vec4  uFogColor;    // rgb, a = strength
uniform vec4  uFogEnv;      // (envStart, envEnd, rdStart, rdEnd)

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    if (any(notEqual(uClipPlane.xyz, vec3(0.0))) &&
        dot(vRenderPos, uClipPlane.xyz) + uClipPlane.w < 0.0) {
        discard;
    }
    vec3 color = vColor.rgb * uEntityLight;
    vec3 fogDelta = vRenderPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    color = mix(color, uFogColor.rgb, fogValue * uFogColor.a);
    FragColor = vec4(color, vColor.a);
}
