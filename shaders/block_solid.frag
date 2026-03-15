// File: shaders/block_solid.frag
// No-discard fragment shader — ZERO discard, ZERO alpha testing.
// Used for the translucent pass (water, ice, stained glass) where blending
// handles transparency and discard is unnecessary. Also suitable for distant
// cutout LOD. Full GPU early-z and Hi-Z optimization enabled.
#version 330 core

in vec2 fragTexCoord;
in vec3 fragWorldPos;
in vec4 fragColor;

uniform sampler2D uTextureAtlas;

out vec4 FragColor;

void main() {
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);
    vec3 finalColor = textureColor.rgb * fragColor.rgb;
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
