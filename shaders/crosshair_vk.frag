#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 FragColor;

void main() {
    vec4 texColor = texture(uTexture, fragTexCoord);
    FragColor = texColor * fragColor;
}
