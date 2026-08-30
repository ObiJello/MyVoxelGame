// File: shaders/block_instanced_vk.vert (Vulkan twin of block_instanced.vert)
//
// Same contract as the GL shader: per-instance transform as ONE vec4 at
// attribute location 3 (xyz world translation, w uniform scale — see
// block_instanced.vert for why that is the whole transform space),
// view-projection in the push-constant uMVP slot (the backend routes the name
// "uViewProj" there), fragment shader = block_vk.frag under the PORTAL
// pipeline layout so the fog/environment CommonUBO is bound.
//
// NOT in CMake's VK_SHADERS list: the committed .spv beside this file is what
// ships. Recompile it by hand after any edit:
//     glslc shaders/block_instanced_vk.vert -o shaders/block_instanced_vk.vert.spv
#version 450

layout (location = 0) in vec3 aPos;       // MODEL-space position
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec4 aColor;     // RGBA8 normalized

// Per-instance: xyz = world translation, w = uniform scale.
layout (location = 3) in vec4 aInstance;

layout (push_constant) uniform PushConstants {
    mat4 uMVP;              // HOLDS uViewProj for this shader (model is per-instance)
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uPortalClipPlane;  // xyz = plane normal, w = -dot(normal, point)
} pc;

layout (location = 0) out vec2 fragTexCoord;
layout (location = 1) out vec3 fragWorldPos;
layout (location = 2) out vec4 fragColor;

out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

void main() {
    vec4 worldPos = vec4(aInstance.xyz + aInstance.w * aPos, 1.0);

    gl_Position  = pc.uMVP * worldPos;
    fragTexCoord = aTexCoord;
    // World-space, from the TRANSFORMED position — aPos is model-space here,
    // unlike block_vk.vert where the mesher already emits world-space.
    fragWorldPos = worldPos.xyz;
    fragColor    = aColor;

    gl_ClipDistance[0] = dot(worldPos.xyz, pc.uPortalClipPlane.xyz) + pc.uPortalClipPlane.w;
}
