// Ported from media/shaders/SpMovie.updb (SpMovie.psh) — the movie-quad shader: samples Y/U/V planes
// (s0/s1/s2) and does YUV->RGB (studio-range BT.601). reg0 = TEXCOORD0. (MovieTolerance branch is
// commented out in the original, so omitted.)
#version 450
layout(location = 0) in vec2 inUV;                              // TEXCOORD0 (reg0)
layout(set = 0, binding = 0) uniform sampler2D YTexture;       // s0
layout(set = 0, binding = 1) uniform sampler2D UTexture;       // s1
layout(set = 0, binding = 2) uniform sampler2D VTexture;       // s2
layout(location = 0) out vec4 outColor;
void main() {
    float Y = texture(YTexture, inUV).r, U = texture(UTexture, inUV).r, V = texture(VTexture, inUV).r;
    vec3 rgb;
    rgb.r = 1.164 * (Y - 0.0625) + 1.596 * (V - 0.5);
    rgb.g = 1.164 * (Y - 0.0625) - 0.391 * (U - 0.5) - 0.813 * (V - 0.5);
    rgb.b = 1.164 * (Y - 0.0625) + 2.018 * (U - 0.5);
    outColor = vec4(rgb, 1.0);
}
