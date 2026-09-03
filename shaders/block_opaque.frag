// File: shaders/block_opaque.frag
// Opaque (solid-layer) fragment shader. NO discard, NO alpha test — like MC's
// terrain.fsh outside ALPHA_CUTOUT. Every fragment lands, alpha ignored, so the
// GPU keeps early-z and the pass runs at pure fill cost.
//
// INVARIANT (enforced by the mesher, not here): nothing routed into the opaque
// layer relies on transparent texels being dropped. The one opaque block that
// did — grass_block's side overlay — has just those quads routed to the cutout
// layer (FaceDef::cutoutOverlay), and every block MC renders in cutout is
// registered Cutout in BlockDefs.inc. Fast-graphics leaves are the deliberate
// case of alpha-0 texels drawn here, which is exactly MC's Fast look.
// Reintroducing a discard would only paper over a block in the wrong layer.
#version 330 core

// Input from vertex shader
in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;

// Uniforms
uniform sampler2D uTextureAtlas;
uniform vec3 uCameraPos;            // World-space camera position (per view)
uniform float uSkyBrightness;       // Day/night terrain dim (0.2667..1)
uniform vec4 uFogColor;             // Time-of-day fog color
uniform vec4 uFogEnv;               // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off

// Output
out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Vertex color contains: biome tint * AO * directional face shade
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    // Day/night sky-light dim + MC-style distance fog
    finalColor *= uSkyBrightness;
    vec3 fogDelta = fragWorldPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    finalColor = mix(finalColor, uFogColor.rgb, fogValue * uFogColor.a);

    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
