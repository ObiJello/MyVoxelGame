#version 330 core
// File: shaders/blockentity.vert (OpenGL block-entity shader)
// Shared by every block-entity renderer — chest, shulker box, bed, skull,
// campfire food, sign board and text — and their item forms (BEWLR). UVs
// arrive normalized (the divide by the sheet size is baked into the meshes).
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), rgba8 (loc 2).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
// The mesh's model matrix into RENDER space (RenderOrigin.hpp) — where the
// fragment sits for the fog. (Named apart from uModel: the shader-pack
// pipeline recognises this program as gbuffers_block by the ABSENCE of a
// uModel.) The item forms draw unfogged and may leave it the identity.
uniform mat4 uLocalToRender;

out vec2 vUV;
out vec4 vColor;
out vec3 vRenderPos;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vRenderPos = (uLocalToRender * vec4(aPos, 1.0)).xyz;
}
