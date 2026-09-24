#version 330 core
// File: shaders/stick_figure.vert (OpenGL) — PlayerRenderer's stick-figure
// players. Flat vertex colour; the geometry is baked in RENDER space on the
// CPU (the portal ghost's pair matrix included), so aPos is where the
// fragment is for the clip plane and the fog alike.
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec3 vRenderPos;
out vec4 vColor;

void main() {
    vRenderPos  = aPos;
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor      = aColor;
}
