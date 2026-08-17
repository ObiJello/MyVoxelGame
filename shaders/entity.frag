#version 330 core

// Mob entity fragment shader (OpenGL).

in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D uTex;

// Hurt flash and the creeper's pre-detonation whiteout, in one uniform:
//   rgb = the colour to blend toward
//   a   = how much of it (0 = untouched)
//
// Named uColor, not uOverlay: VKBackend::SetUniformVec4 routes vec4 uniforms by
// NAME into a fixed push-constant slot, and only uTint/uColor/uClipPlane/
// uPortalClipPlane are recognised. A shader here that declared uOverlay would
// work on OpenGL and silently render unlit on Vulkan.
uniform vec4 uColor;

void main() {
    vec4 t = texture(uTex, vUV);

    // Entity textures are cutout, not blended: the transparent regions of a
    // 64x32 sheet must be discarded, or every mob renders inside a black box.
    if (t.a < 0.05) discard;

    vec3 base = t.rgb * vColor.rgb;
    FragColor = vec4(mix(base, uColor.rgb, uColor.a), 1.0);
}
