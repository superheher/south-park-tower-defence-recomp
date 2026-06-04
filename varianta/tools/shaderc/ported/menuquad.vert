#version 450
// GPU-build piece 3b: generic UI vertex shader. The menu draws are float2 (x,y) screen-space positions
// (8-byte stride, RE'd from the vertex pool), transformed by the title's $worldviewProj matrix (read from
// the ALU constants, reg 0x4000) into clip space. Color is a per-draw constant (push constant for now).
layout(location = 0) in vec2 inPos;
layout(push_constant) uniform PC { mat4 mvp; vec4 color; } pc;
layout(location = 0) out vec4 vColor;
void main() { gl_Position = pc.mvp * vec4(inPos, 0.0, 1.0); vColor = pc.color; }
