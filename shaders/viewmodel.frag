// File: shaders/viewmodel.frag
// Per-fragment shading for the portal-gun viewmodel.
// Cheap forward shade — Portal's actual VertexLitGeneric uses a
// per-pixel lightwarp + half-Lambert, but for a hand-rendered viewmodel
// with no world-space lighting source we approximate it with a fixed
// "studio-light" diffuse term: a head-on key plus a slight ambient
// fill so the unlit side doesn't go pure black. This matches the way
// Source's viewmodels render in flat / non-lightmapped maps (e.g. the
// portalgun test chambers — see view_scene.cpp's viewmodel pass).

#version 330 core

uniform sampler2D uDiffuse;
uniform float     uAlphaCutoff;   // 0 = no cutout; >0 = discard if a < cutoff
uniform vec3      uKeyDir;        // unit vector, camera-space (typically (0,0,-1))
uniform float     uKeyIntensity;  // 0..1.5 — controls highlight strength
uniform float     uAmbient;       // 0..1 — flat fill so back faces aren't black

in vec2 vUV;
in vec3 vWorldNormal;

out vec4 FragColor;

void main() {
    vec4 tex = texture(uDiffuse, vUV);
    if (uAlphaCutoff > 0.0 && tex.a < uAlphaCutoff) discard;

    vec3 n      = normalize(vWorldNormal);
    // Half-Lambert (Source idiom): NdotL * 0.5 + 0.5, then squared.
    // Softens the terminator and avoids pure-black backfaces — exactly
    // how Portal's stdshader lights this model.
    float ndl   = max(dot(n, -normalize(uKeyDir)), 0.0);
    float halfL = pow(ndl * 0.5 + 0.5, 2.0);
    float shade = clamp(uAmbient + uKeyIntensity * halfL, 0.0, 1.5);

    FragColor = vec4(tex.rgb * shade, tex.a);
}
