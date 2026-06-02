// Ported from media/shaders/SimpleCol.updb — vertex color * a material-color constant (uniform float4 matCol).
// The constant comes from an ALU constant register (SET_CONSTANT) -> push constant.
#version 450
layout(location = 0) in vec4 inColor;   // COLOR (reg0)
layout(push_constant) uniform PC { vec4 matCol; } pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = inColor * pc.matCol; }
