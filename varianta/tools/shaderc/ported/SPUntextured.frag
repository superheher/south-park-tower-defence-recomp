// Ported from media/shaders/SPUntextured.updb — output the interpolated vertex color. reg0 = COLOR.
#version 450
layout(location = 0) in vec4 inColor;   // COLOR (reg0)
layout(location = 0) out vec4 outColor;
void main() { outColor = inColor; }
