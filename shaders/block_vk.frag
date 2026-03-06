// File: shaders/block_vk.frag (Vulkan version)
#version 450

// Input from vertex shader
layout (location = 0) in vec3 fragNormal;
layout (location = 1) in vec2 fragTexCoord;
layout (location = 2) in vec3 fragWorldPos;
layout (location = 3) in vec4 fragColor;

// Texture atlas sampler (descriptor set 0, binding 0)
layout (set = 0, binding = 0) uniform sampler2D uTextureAtlas;

// Output
layout (location = 0) out vec4 FragColor;

void main() {
    // Sample the texture atlas
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard fully transparent pixels
    if (textureColor.a < 0.1) {
        discard;
    }

    // Use vertex color for biome tinting
    vec3 biomeTint = fragColor.rgb;

    // Simple directional lighting
    vec3 normal = normalize(fragNormal);
    vec3 sunDir = normalize(vec3(0.3, -0.8, 0.2));
    vec3 sunCol = vec3(1.0, 1.0, 0.9);
    vec3 ambientCol = vec3(0.4, 0.5, 0.7);
    float ambientStr = 0.3;

    float NdotL = max(dot(normal, -sunDir), 0.15);
    vec3 diffuse = sunCol * NdotL;
    vec3 ambient = ambientCol * ambientStr;
    vec3 lighting = ambient + diffuse;

    vec3 finalColor = textureColor.rgb * lighting * biomeTint;
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}
