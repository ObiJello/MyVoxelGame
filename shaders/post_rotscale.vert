#version 330 core
// File: shaders/post_rotscale.vert (OpenGL) — MC post/rotscale.vsh: the
// full-screen quad plus a rotated, scaled copy of its texture coordinate.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

out vec2 texCoord;
out vec2 scaledCoord;

uniform mat4 uPostParams;
#define P0 uPostParams[0]
#define P1 uPostParams[1]

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    texCoord = aUV;

    // RotScaleConfig { vec2 InScale; vec2 InOffset; float InRotation; }
    vec2 InScale = P0.xy;
    vec2 InOffset = P0.zw;
    float InRotation = P1.x;
    float Deg2Rad = 0.0174532925;
    float InRadians = InRotation * Deg2Rad;
    float Cosine = cos(InRadians);
    float Sine = sin(InRadians);
    float RotU = texCoord.x * Cosine - texCoord.y * Sine;
    float RotV = texCoord.y * Cosine + texCoord.x * Sine;
    scaledCoord = vec2(RotU, RotV) * InScale + InOffset;
}
