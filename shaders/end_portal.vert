// File: shaders/end_portal.vert
//
// Mojang's core/rendertype_end_portal.vsh with its three #moj_imports
// hand-substituted (the vendored original is at
// assets/shaders/core/rendertype_end_portal.vsh; the helpers it imports are
// under assets/shaders/include/). Companion to EndPortalRenderer.{hpp,cpp}
// and to shaders/end_portal_vk.vert, which must stay in step with it.
#version 330 core

// The shared 24-byte block vertex layout (GetBlockVertexLayout). MC's
// END_PORTAL pipeline takes DefaultVertexFormat.POSITION and nothing else
// (RenderPipelines.java:154) — everything the fragment stage needs is derived
// from the clip-space position. We keep the engine's standard layout anyway,
// because it is the format both backends set up by default; the quad builder
// pads each vertex with a dead UV + colour rather than registering a private
// vertex layout for three floats.
layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;

out vec4 vTexProj;    // Mojang's texProj0
out vec3 vWorldPos;   // for the engine's fragment-side fog

// assets/shaders/include/projection.glsl, verbatim. This turns the clip-space
// position into the projective texture coordinate the fragment stage feeds to
// textureProj — which is what makes the starfield SCREEN-locked instead of
// wrapped onto the quad. Sampling by a normal UV would give a flat texture
// that slides with the block; this gives the "window into space" look.
vec4 projection_from_position(vec4 position) {
    vec4 projection = position * 0.5;
    projection.xy = vec2(projection.x + projection.w, projection.y + projection.w);
    projection.zw = position.zw;
    return projection;
}

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);

    vTexProj = projection_from_position(gl_Position);

    // MC computes its two fog distances here, from a Position that is already
    // camera-relative. This engine's terrain shaders instead carry the world
    // position through and do the camera subtraction in the fragment stage
    // (shaders/block_solid.frag) — matched here so a portal seen through fog
    // fades at exactly the same rate as the stronghold bricks around it.
    vWorldPos = aPos;
}
