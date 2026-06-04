#version 450
// GPU-build piece 3b: flat-color UI fragment shader (first pass — proves the menu-quad pipeline + layout).
// Later: swap for the hand-ported per-shader FS (Simple/SPTextured/...) with the sampled texture * color.
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vColor; }
