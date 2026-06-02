// Ported from media/shaders/Simple.updb (Simple.psh, ps_3_0):
//   float4 main_ps(PS_IN In, uniform sampler2D diffuseTexture) : COLOR
//   { return tex2D(diffuseTexture, In.Tex) * In.Color; }
// Interpolators (.updb): reg0 = COLOR (0xa0), reg1 = TEXCOORD0 (0x50). Constant: s0 = diffuseTexture.
#version 450
layout(location = 0) in vec4 inColor;   // COLOR     (interpolator register 0)
layout(location = 1) in vec2 inTex;     // TEXCOORD0 (interpolator register 1)
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;   // sampler s0
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(diffuseTexture, inTex) * inColor; }
