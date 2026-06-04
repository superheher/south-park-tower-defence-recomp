#version 450
// GPU-build piece 1: self-contained test triangle (no vertex buffer — positions from gl_VertexIndex).
// Proves the variant-A graphics pipeline (render pass -> pipeline -> vkCmdDraw -> swapchain) end-to-end,
// the foundation the real DRAW_INDX->Vulkan translator plugs into.
layout(location = 0) out vec3 vColor;
vec2 P[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
vec3 C[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.4, 1.0));
void main() { gl_Position = vec4(P[gl_VertexIndex], 0.0, 1.0); vColor = C[gl_VertexIndex]; }
