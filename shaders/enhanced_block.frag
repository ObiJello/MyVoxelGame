// File: shaders/enhanced_block.frag (Updated with Color Support)
#version 330 core

// Input from vertex shader
in vec3 fragNormal;     // Interpolated normal
in vec2 fragTexCoord;   // Interpolated texture coordinates
in vec3 fragWorldPos;   // World position
in vec4 fragColor;      // Interpolated vertex color (NEW)

// Uniforms
uniform sampler2D uTextureAtlas;     // The main texture atlas
uniform sampler2D uGrassColormap;    // Grass biome colormap (256x256)
uniform sampler2D uFoliageColormap;  // Foliage biome colormap (256x256)

// Biome parameters (could be per-vertex in the future)
uniform float uBiomeTemperature;     // 0.0-1.0 range
uniform float uBiomeHumidity;        // 0.0-1.0 range
uniform int uEnableBiomeTinting;     // 0 = disabled, 1 = enabled

// Output
out vec4 FragColor;

void main() {
    // Sample the texture atlas
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard fully transparent pixels
    if (textureColor.a < 0.1) {
        discard;
    }

    // OPTION 1: Use vertex color (preferred - pre-calculated biome tinting)
    vec3 biomeTint = fragColor.rgb;

    // Simple directional lighting calculation
    vec3 normal = normalize(fragNormal);

    // Default sun direction (pointing down and slightly south-east)
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

    // Apply lighting and biome tinting to texture color
    vec3 finalColor = textureColor.rgb * lighting * biomeTint;

    // Output final color with original alpha
    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
}