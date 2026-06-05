#version 450
// Textured UI vertex shader (disk-resource path, cont.23). The menu draws carry pos.xy (screen/local)
// + uv.xy per vertex (RE'd: text fills are stride-16 pos+uv; sprite fills similar). Transformed by the
// title's worldviewProj (reg 0x4000 ALU consts → push-const mvp); passes uv + per-draw color to the
// textured FS (Simple.frag / SPTextured.frag = texture(diffuse, uv) * color).
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inTex;
layout(push_constant) uniform PC { mat4 mvp; vec4 color; } pc;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTex;
void main() {
    gl_Position = pc.mvp * vec4(inPos, 0.0, 1.0);
    vColor = pc.color;
    vTex   = inTex;
}
