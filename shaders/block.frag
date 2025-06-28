#version 330 core

// Input from vertex shader
in vec3 fragNormal;     // Interpolated normal
in vec2 fragTexCoord;   // Interpolated texture coordinates

// Uniforms
uniform sampler2D uTextureAtlas;  // The texture atlas

// Output
out vec4 FragColor;

void main() {
    // Sample the texture atlas
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard fully transparent pixels (for air blocks or transparent textures)
    if (textureColor.a < 0.1) {
        discard;
    }

    // Simple directional lighting calculation
    vec3 normal = normalize(fragNormal);

    // Default sun direction (pointingg down and slightly south-east)
    vec3 sunDir = normalize(vec3(0.3, -0.8, 0.2));
    vec3 sunCol = vec3(1.0, 1.0, 0.9);  // Warm white sunlight
    vec3 ambientCol = vec3(0.4, 0.5, 0.7);  // Cool blue ambient
    float ambientStr = 0.3;

    // Calculate diffuse lighting
    float NdotL = max(dot(normal, -sunDir), 0.0);
    vec3 diffuse = sunCol * NdotL;

    // Calculate ambient lighting
    vec3 ambient = ambientCol * ambientStr;

    // Combine lighting
    vec3 lighting = ambient + diffuse;

    // Apply lighting to texture color
    vec3 finalColor = textureColor.rgb * lighting;

    // Output final color with original alpha
    FragColor = vec4(finalColor, textureColor.a);
}