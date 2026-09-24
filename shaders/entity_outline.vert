#version 330 core
// File: shaders/entity_outline.vert (OpenGL) — MC rendertype_outline.vsh.
// A glowing entity's geometry drawn flat into the entity-outline target
// (EntityOutline.hpp). The vertices are whatever the entity's own renderer
// uploaded; uMVP is the matrix it drew them with.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
}
