#version 330 core

in vec3 vNormal;
out vec4 FragColor;

void main() {
    // Simple directional “lighting” toward (1,1,1)
    vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
    float intensity = max(dot(normalize(vNormal), lightDir), 0.0);
    vec3 baseColor = vec3(0.6, 0.7, 0.8);
    vec3 color = baseColor * (0.3 + 0.7 * intensity);
    FragColor = vec4(color, 1.0);
}
