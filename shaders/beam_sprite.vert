#version 330 core
// File: shaders/beam_sprite.vert (OpenGL volumetric light beam — glare / light-pool quad)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;     // unused (block vertex layout)
layout(location = 2) in vec4 aColor;  // unused
uniform mat4 uMVP;
uniform mat4 uLocalToRender;   // the sprite's model matrix into render space
#define MVP uMVP
#define LOCAL_TO_RENDER(p) (uLocalToRender * (p)).xyz
out vec3 vLocal;
out vec3 vRenderPos;

void main() {
    vec4 p = vec4(aPos, 1.0);
    gl_Position = MVP * p;
    vLocal = aPos;
    vRenderPos = LOCAL_TO_RENDER(p);
}
