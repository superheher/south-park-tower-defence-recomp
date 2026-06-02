// Ported from media/shaders/SPTextured.updb — texture * vertex color. reg0 = COLOR, reg1 = TEXCOORD0, s0.
#version 450
layout(location = 0) in vec4 inColor;   // COLOR     (reg0)
layout(location = 1) in vec2 inTex;     // TEXCOORD0 (reg1)
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;   // s0
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(diffuseTexture, inTex) * inColor; }
