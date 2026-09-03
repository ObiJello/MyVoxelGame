#version 330 core

// Mob entity vertex shader (OpenGL).
//
// Vertices arrive ALREADY IN WORLD SPACE: ModelPart::Build applies the part
// hierarchy and the entity's world matrix on the CPU. That is deliberate —
// it lets every mob sharing a texture batch into one draw call, and it keeps
// the shader to a single matrix uniform, which is what the Vulkan backend's
// push-constant budget allows (see VKBackend::SetUniformMat4).

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
// Portal clip plane in the same space as aPos (camera-relative world; the
// renderer folds the camera offset into .w). Zero = no clipping. Its own
// name rather than uPortalClipPlane: on Vulkan that name lands in the
// push-constant slot this shader uses for the hurt overlay (uColor).
uniform vec4 uEntityClipPlane;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(uEntityClipPlane.xyz, vec3(0.0))))
        ? dot(uEntityClipPlane.xyz, aPos) + uEntityClipPlane.w
        : 1.0;
}
