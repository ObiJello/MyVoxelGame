#pragma once
namespace console {
inline constexpr const char* terrainVertexShader=R"(#version 150
in vec3 position; in vec2 texcoord; in vec4 color; in vec2 lightcoord;
uniform mat4 matrix; uniform vec3 eye;
out vec2 uv; out vec4 tint; out float distanceToEye; out vec2 lightUV;
void main(){gl_Position=matrix*vec4(position,1);uv=texcoord;tint=color;lightUV=lightcoord;distanceToEye=length(position-eye);})";
inline constexpr const char* terrainFragmentShader=R"(#version 150
in vec2 uv;in vec4 tint;in float distanceToEye;in vec2 lightUV;
uniform sampler2D atlas;uniform sampler2D lightmap;uniform bool useLighting;uniform bool skyPass;uniform float fogDistance;uniform vec3 fogColor;
out vec4 fragment;
void main(){vec4 c=texture(atlas,uv)*tint;if(!skyPass && c.a<0.1)discard;
if(useLighting)c.rgb*=texture(lightmap,lightUV).rgb;
float fog=fogDistance>0?smoothstep(fogDistance*.45,fogDistance,distanceToEye):0;
fragment=vec4(mix(c.rgb,fogColor,fog),c.a);})";
}
