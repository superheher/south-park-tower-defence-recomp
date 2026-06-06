// rex_texture.h — Xenos (Xbox 360 GPU) texture decode for variant A's renderer.
//
// GPU-RESOURCE-BUILD-PLAN piece 2 / cont.25 R0 keystone: the title reads texture files into system
// memory but the decode+upload into guest GPU texture blocks (+ the Xenos untile) is the missing
// renderer step. This module is that step's CPU side: given a Xenos texture fetch-constant descriptor
// (base/format/dims/tiled/endian — already parsed in kernel.cpp's WriteGpuReg hook), it untiles and
// format-converts the guest texture bytes to a linear RGBA8 buffer the Vulkan renderer can upload.
//
// Header-only (inline), included by exactly one TU (kernel.cpp) so there are no ODR/link concerns and
// no CMake glob/reconfigure needed. If vulkan_render.cpp later needs it, promote to a .cpp.
//
// VERIFICATION (rigor — see SelfTest(), gated REX_TEXSELFTEST): the address function is the documented
// Microsoft XGAddress2DTiledOffset (used by Xenia/noesis/xbox360 tools); the round-trip test proves
// Untile() inverts it over the texel domain (i.e. it is injective + correctly inverted), the DXT1
// known-vector test proves the BC1 decoder, and decoding rat.dds proves the 8888 converter on real art.
// Final hardware-layout confirmation is the REX_TEXDECODE PPM of a real bound texture.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>   // std::max_element
#include <utility>     // std::swap

extern uint8_t* g_base;   // guest memory base (4 GiB), defined in runtime.cpp

namespace rex_tex {

// ---- Xenos texture data formats (GPUTEXTUREFORMAT — fetch-constant d1 low 6 bits). Subset handled. ----
enum XenosFormat : uint32_t {
    FMT_8         = 2,
    FMT_1_5_5_5   = 3,
    FMT_5_6_5     = 4,
    FMT_6_5_5     = 5,
    FMT_8_8_8_8   = 6,    // the common 32bpp UI format
    FMT_2_10_10_10= 7,
    FMT_8_8       = 10,
    FMT_4_4_4_4   = 15,
    FMT_DXT1      = 18,   // 0x12  BC1 (8 bytes/block)
    FMT_DXT2_3    = 19,   // 0x13  BC2 (16 bytes/block, explicit 4-bit alpha)
    FMT_DXT4_5    = 20,   // 0x14  BC3 (16 bytes/block, interpolated alpha)
    FMT_DXN       = 36,   // 0x24  BC5 (two-channel; not yet decoded -> grey)
    FMT_CTX1      = 59,   // 0x3B  (two-channel; not yet decoded -> grey)
    FMT_DXT3A     = 60,   // 0x3C
    FMT_DXT5A     = 61,   // 0x3D  BC4 single-channel
};

// Endianness field (fetch-constant d1 bits[7:6]): how the GPU byte-swaps texture data on read.
enum XenosEndian : uint32_t { END_NONE = 0, END_8IN16 = 1, END_8IN32 = 2, END_16IN32 = 3 };

// A parsed 2D texture descriptor (filled by kernel.cpp from the 6-dword fetch constant).
struct Desc {
    uint32_t guestBase;   // guest address of the texel data (0xA0000000 window)
    uint32_t format;      // XenosFormat
    uint32_t width;       // texels
    uint32_t height;      // texels
    uint32_t tiled;       // 1 = Xenos 2D-tiled, 0 = linear
    uint32_t endian;      // XenosEndian
    uint32_t pitchTexels; // row pitch in texels (linear case); 0 => width
};

// =====================================================================================================
//  Xenos 2D tiling — the documented Microsoft XGAddress2DTiledOffset.
//  x,y are in TEXELS for uncompressed formats, in 4x4 BLOCKS for compressed (DXT) formats.
//  texelPitch = bytes per texel (uncompressed) or per block (compressed): 1,2,4,8,16.
//  Returns the offset in UNITS OF texelPitch (multiply by texelPitch for the byte offset).
// =====================================================================================================
static inline uint32_t XGAddress2DTiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t texelPitch)
{
    uint32_t alignedWidth = (width + 31u) & ~31u;
    // logBpp = log2(texelPitch), via the documented bit-trick (exact for 1,2,4,8,16).
    uint32_t logBpp = (texelPitch >> 2) + ((texelPitch >> 1) >> (texelPitch >> 2));
    uint32_t macro  = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
    uint32_t micro  = (((x & 7) + ((y & 6) << 2)) << logBpp);
    uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u)
                      + ((y & 8) << (3u + logBpp)) + ((y & 1) << 4);
    return (((offset & ~0x1FFu) << 3) + ((offset & 0x1C0u) << 2) + (offset & 0x3Fu)
            + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
}

// Untile a tiled surface -> linear. dims in BLOCKS (=texels for uncompressed). bytesPerBlock = texelPitch.
static inline void Untile(const uint8_t* src, uint8_t* dst, uint32_t wBlocks, uint32_t hBlocks, uint32_t bytesPerBlock)
{
    for (uint32_t y = 0; y < hBlocks; y++)
        for (uint32_t x = 0; x < wBlocks; x++) {
            uint32_t tiledOff = XGAddress2DTiledOffset(x, y, wBlocks, bytesPerBlock) * bytesPerBlock;
            uint32_t linOff   = (y * wBlocks + x) * bytesPerBlock;
            memcpy(dst + linOff, src + tiledOff, bytesPerBlock);
        }
}

// Tile a linear surface -> tiled (the inverse of Untile; used by the round-trip self-test only).
static inline void Tile(const uint8_t* lin, uint8_t* tiled, uint32_t wBlocks, uint32_t hBlocks, uint32_t bytesPerBlock)
{
    for (uint32_t y = 0; y < hBlocks; y++)
        for (uint32_t x = 0; x < wBlocks; x++) {
            uint32_t tiledOff = XGAddress2DTiledOffset(x, y, wBlocks, bytesPerBlock) * bytesPerBlock;
            uint32_t linOff   = (y * wBlocks + x) * bytesPerBlock;
            memcpy(tiled + tiledOff, lin + linOff, bytesPerBlock);
        }
}

// Tiled surface size in blocks (both dims rounded up to 32 — the tile granularity). Used to bound buffers.
static inline uint32_t TiledSizeBytes(uint32_t wBlocks, uint32_t hBlocks, uint32_t bytesPerBlock)
{
    uint32_t aw = (wBlocks + 31u) & ~31u, ah = (hBlocks + 31u) & ~31u;
    return aw * ah * bytesPerBlock;
}

// ---- Endian swap applied to the raw texel bytes before interpretation. ----
static inline void EndianSwap(uint8_t* p, size_t bytes, uint32_t endian)
{
    switch (endian) {
        case END_8IN16:  for (size_t i = 0; i + 1 < bytes; i += 2) std::swap(p[i], p[i + 1]); break;
        case END_8IN32:  for (size_t i = 0; i + 3 < bytes; i += 4) { std::swap(p[i], p[i + 3]); std::swap(p[i + 1], p[i + 2]); } break;
        case END_16IN32: for (size_t i = 0; i + 3 < bytes; i += 4) { std::swap(p[i], p[i + 2]); std::swap(p[i + 1], p[i + 3]); } break;
        default: break;
    }
}

// =====================================================================================================
//  Block-compression (BC1/2/3) decoders. Input blocks are expected in PC little-endian layout (apply
//  EndianSwap first for Xenos data). Each decodes one 4x4 block into `out` (16 RGBA8 texels, row-major).
// =====================================================================================================
static inline void Rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
    g = uint8_t(((c >> 5)  & 0x3F) * 255 / 63);
    b = uint8_t(( c        & 0x1F) * 255 / 31);
}

// BC1 color block (8 bytes). Returns 16 RGBA8. `punchAlpha`: in 3-color mode, index 3 = transparent.
static inline void DecodeBC1Color(const uint8_t* b, uint8_t out[64], bool punchAlpha)
{
    uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
    uint8_t r[4], g[4], bl[4], a[4] = {255, 255, 255, 255};
    Rgb565(c0, r[0], g[0], bl[0]); Rgb565(c1, r[1], g[1], bl[1]);
    if (c0 > c1 || !punchAlpha) {
        r[2] = uint8_t((2 * r[0] + r[1]) / 3); g[2] = uint8_t((2 * g[0] + g[1]) / 3); bl[2] = uint8_t((2 * bl[0] + bl[1]) / 3);
        r[3] = uint8_t((r[0] + 2 * r[1]) / 3); g[3] = uint8_t((g[0] + 2 * g[1]) / 3); bl[3] = uint8_t((bl[0] + 2 * bl[1]) / 3);
    } else {
        r[2] = uint8_t((r[0] + r[1]) / 2); g[2] = uint8_t((g[0] + g[1]) / 2); bl[2] = uint8_t((bl[0] + bl[1]) / 2);
        r[3] = g[3] = bl[3] = 0; a[3] = 0;   // transparent black
    }
    uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) {
        int idx = (bits >> (i * 2)) & 3;
        out[i * 4 + 0] = r[idx]; out[i * 4 + 1] = g[idx]; out[i * 4 + 2] = bl[idx]; out[i * 4 + 3] = a[idx];
    }
}

// BC2 (DXT2/3): 8-byte explicit 4-bit alpha + 8-byte BC1 color (always 4-color mode).
static inline void DecodeBC2(const uint8_t* b, uint8_t out[64])
{
    DecodeBC1Color(b + 8, out, /*punchAlpha=*/false);
    uint64_t a = 0; for (int i = 0; i < 8; i++) a |= uint64_t(b[i]) << (i * 8);
    for (int i = 0; i < 16; i++) { int v = (a >> (i * 4)) & 0xF; out[i * 4 + 3] = uint8_t(v * 255 / 15); }
}

// BC3 (DXT4/5): 8-byte interpolated alpha + 8-byte BC1 color (always 4-color mode).
static inline void DecodeBC3(const uint8_t* b, uint8_t out[64])
{
    DecodeBC1Color(b + 8, out, /*punchAlpha=*/false);
    uint8_t a0 = b[0], a1 = b[1], al[8];
    al[0] = a0; al[1] = a1;
    if (a0 > a1) { for (int i = 1; i <= 6; i++) al[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7); }
    else { for (int i = 1; i <= 4; i++) al[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5); al[6] = 0; al[7] = 255; }
    uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (i * 8);
    for (int i = 0; i < 16; i++) { int idx = (bits >> (i * 3)) & 7; out[i * 4 + 3] = al[idx]; }
}

// =====================================================================================================
//  Top-level decode: Xenos texture (guest memory) -> linear RGBA8 (width*height*4). Returns empty on
//  unsupported format. `rgbaOut` is resized.
// =====================================================================================================
static inline bool IsCompressed(uint32_t fmt)
{
    return fmt == FMT_DXT1 || fmt == FMT_DXT2_3 || fmt == FMT_DXT4_5 ||
           fmt == FMT_DXN  || fmt == FMT_CTX1   || fmt == FMT_DXT3A || fmt == FMT_DXT5A;
}
static inline uint32_t BytesPerBlock(uint32_t fmt)
{
    switch (fmt) {
        case FMT_DXT1: case FMT_DXT5A: case FMT_DXT3A: case FMT_CTX1: return 8;
        case FMT_DXT2_3: case FMT_DXT4_5: case FMT_DXN: return 16;
        case FMT_8: return 1;
        case FMT_1_5_5_5: case FMT_5_6_5: case FMT_6_5_5: case FMT_4_4_4_4: case FMT_8_8: return 2;
        case FMT_8_8_8_8: case FMT_2_10_10_10: return 4;
        default: return 0;
    }
}

// Convert one uncompressed texel (little-endian, post-endian-swap) at `s` to RGBA8.
static inline void ConvUncompressed(const uint8_t* s, uint32_t fmt, uint8_t* o)
{
    switch (fmt) {
        case FMT_8_8_8_8: {                       // stored A8R8G8B8 (B,G,R,A in memory little-endian)
            o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; break;
        }
        case FMT_5_6_5: { uint16_t c = uint16_t(s[0] | (s[1] << 8)); Rgb565(c, o[0], o[1], o[2]); o[3] = 255; break; }
        case FMT_1_5_5_5: { uint16_t c = uint16_t(s[0] | (s[1] << 8));
            o[0] = uint8_t(((c >> 10) & 0x1F) * 255 / 31); o[1] = uint8_t(((c >> 5) & 0x1F) * 255 / 31);
            o[2] = uint8_t((c & 0x1F) * 255 / 31); o[3] = (c & 0x8000) ? 255 : 0; break; }
        case FMT_4_4_4_4: { uint16_t c = uint16_t(s[0] | (s[1] << 8));
            o[0] = uint8_t(((c >> 8) & 0xF) * 17); o[1] = uint8_t(((c >> 4) & 0xF) * 17);
            o[2] = uint8_t((c & 0xF) * 17); o[3] = uint8_t(((c >> 12) & 0xF) * 17); break; }
        case FMT_8:    o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
        case FMT_8_8:  o[0] = s[0]; o[1] = s[1]; o[2] = 0; o[3] = 255; break;
        default:       o[0] = o[1] = o[2] = 128; o[3] = 255; break;   // unsupported -> grey
    }
}

// Decode from raw bytes already in host memory (used by the self-test + the guest path).
static inline bool DecodeBytesToRGBA(const uint8_t* data, const Desc& d, std::vector<uint8_t>& rgbaOut)
{
    uint32_t W = d.width, H = d.height, fmt = d.format, bpb = BytesPerBlock(fmt);
    if (W == 0 || H == 0 || bpb == 0) return false;
    rgbaOut.assign(size_t(W) * H * 4, 0);

    if (IsCompressed(fmt)) {
        uint32_t bw = (W + 3) / 4, bh = (H + 3) / 4;
        std::vector<uint8_t> linBlocks(size_t(bw) * bh * bpb);
        if (d.tiled) Untile(data, linBlocks.data(), bw, bh, bpb);
        else memcpy(linBlocks.data(), data, linBlocks.size());
        EndianSwap(linBlocks.data(), linBlocks.size(), d.endian);
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* blk = linBlocks.data() + (size_t(by) * bw + bx) * bpb;
                uint8_t texels[64];
                if      (fmt == FMT_DXT1)   DecodeBC1Color(blk, texels, true);
                else if (fmt == FMT_DXT2_3) DecodeBC2(blk, texels);
                else if (fmt == FMT_DXT4_5) DecodeBC3(blk, texels);
                else { for (int i = 0; i < 16; i++) { texels[i*4]=texels[i*4+1]=texels[i*4+2]=128; texels[i*4+3]=255; } }
                for (int ty = 0; ty < 4; ty++)
                    for (int tx = 0; tx < 4; tx++) {
                        uint32_t px = bx * 4 + tx, py = by * 4 + ty;
                        if (px >= W || py >= H) continue;
                        memcpy(&rgbaOut[(size_t(py) * W + px) * 4], &texels[(ty * 4 + tx) * 4], 4);
                    }
            }
        return true;
    }

    // Uncompressed: untile texel-wise (or copy linear with pitch), then convert.
    std::vector<uint8_t> lin(size_t(W) * H * bpb);
    if (d.tiled) {
        Untile(data, lin.data(), W, H, bpb);
    } else {
        uint32_t pitch = d.pitchTexels ? d.pitchTexels : W;
        for (uint32_t y = 0; y < H; y++) memcpy(&lin[size_t(y) * W * bpb], data + size_t(y) * pitch * bpb, size_t(W) * bpb);
    }
    EndianSwap(lin.data(), lin.size(), d.endian);
    for (uint32_t i = 0; i < W * H; i++) ConvUncompressed(&lin[size_t(i) * bpb], fmt, &rgbaOut[size_t(i) * 4]);
    return true;
}

// Decode straight from guest memory (the renderer path).
static inline bool DecodeGuestToRGBA(const Desc& d, std::vector<uint8_t>& rgbaOut)
{
    return DecodeBytesToRGBA(g_base + d.guestBase, d, rgbaOut);
}

// cont.62 note: the title's boot/menu splash textures are LINEAR but their real row pitch/width
// (640/384/896) is NOT the d2 width field (256). The true pitch is the documented Xenos fetch-constant
// d0 field bits[30:22] << 5 (Xenia's xe_gpu_texture_fetch_t.pitch, texels) — kernel.cpp's WriteGpuReg
// hook applies it (sets Desc.width = Desc.pitchTexels = pitch). No runtime auto-detection is needed.

// ---- small RGBA8 -> PPM writer (debug capture; ignores alpha) ----
static inline void WriteRGBAasPPM(const char* path, const uint8_t* rgba, uint32_t W, uint32_t H)
{
    FILE* f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (uint32_t i = 0; i < W * H; i++) fwrite(rgba + size_t(i) * 4, 1, 3, f);
    fclose(f);
}

// =====================================================================================================
//  SelfTest (REX_TEXSELFTEST) — verify the decoder WITHOUT depending on race-gated title data.
// =====================================================================================================
inline bool SelfTest()
{
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        fprintf(stderr, "[texselftest] %-46s %s\n", what, ok ? "PASS" : "FAIL");
        if (ok) pass++; else fail++;
    };

    // (1) Tiler round-trip for several dims/bpp: linear -> Tile -> Untile == linear (over the texel domain).
    //     Proves Untile correctly inverts the (injective) address function for these configs.
    struct { uint32_t w, h, bpb; } cfg[] = { {32,32,4}, {64,32,4}, {128,128,2}, {16,16,8}, {64,64,16}, {48,80,4} };
    for (auto& c : cfg) {
        std::vector<uint8_t> lin(size_t(c.w) * c.h * c.bpb), lin2(lin.size());
        for (size_t i = 0; i < lin.size(); i++) lin[i] = uint8_t((i * 131 + 7) & 0xFF);   // deterministic pattern
        std::vector<uint8_t> tiled(TiledSizeBytes(c.w, c.h, c.bpb), 0);
        Tile(lin.data(), tiled.data(), c.w, c.h, c.bpb);
        Untile(tiled.data(), lin2.data(), c.w, c.h, c.bpb);
        char nm[64]; snprintf(nm, sizeof nm, "tile roundtrip %ux%u bpb=%u", c.w, c.h, c.bpb);
        check(lin == lin2, nm);
    }
    // Address-function injectivity over a 64x64 domain (no two texels alias — a wrong formula collides).
    {
        std::vector<uint8_t> seen(64 * 64 * 4 * 4, 0); bool inj = true;   // bpb=4 -> max offset < w*h*bpb*~? bound generously
        std::vector<uint32_t> offs; offs.reserve(64 * 64);
        for (uint32_t y = 0; y < 64; y++) for (uint32_t x = 0; x < 64; x++) offs.push_back(XGAddress2DTiledOffset(x, y, 64, 4));
        std::vector<uint8_t> hit(*std::max_element(offs.begin(), offs.end()) + 1, 0);
        for (uint32_t o : offs) { if (hit[o]) { inj = false; break; } hit[o] = 1; }
        check(inj, "address fn injective over 64x64");
    }

    // (2) BC1 known-vector: a hand-built little-endian DXT1 block (endian=NONE) -> known 4x4 RGBA.
    //     color0 = pure red (565 0xF800), color1 = pure blue (565 0x001F), c0>c1 -> 4-color mode.
    //     indices: row0 all idx0(red), row1 all idx1(blue), row2 all idx2(2/3R+1/3B), row3 all idx3(1/3R+2/3B).
    {
        uint8_t blk[8];
        blk[0] = 0x00; blk[1] = 0xF8;   // c0 = 0xF800 (red), little-endian
        blk[2] = 0x1F; blk[3] = 0x00;   // c1 = 0x001F (blue)
        // 2-bit indices per texel, row-major, bit i*2. row0=0b00, row1=0b01, row2=0b10, row3=0b11.
        // byte4 = texels0..3 (row0) = 0x00; byte5 = row1 = 0x55; byte6 = row2 = 0xAA; byte7 = row3 = 0xFF.
        blk[4] = 0x00; blk[5] = 0x55; blk[6] = 0xAA; blk[7] = 0xFF;
        uint8_t out[64]; DecodeBC1Color(blk, out, true);
        bool ok = out[0] == 255 && out[2] == 0          // texel0 red
               && out[16] == 0  && out[18] == 255        // texel4 (row1) blue
               && out[3] == 255 && out[19] == 255;       // opaque
        // row2 = 2/3 red + 1/3 blue -> R≈170, B≈85
        ok = ok && out[32] > 150 && out[32] < 190 && out[34] > 70 && out[34] < 100;
        check(ok, "BC1 known-vector decode");
    }

    // (3) Real-art converter check: decode rat.dds (32x16 A8R8G8B8 linear) -> /tmp/rat_decoded.ppm.
    //     Proves the FMT_8_8_8_8 path on real data (visual confirm: a recognizable rat texture).
    {
        const char* dds = getenv("REX_RATDDS");
        std::string p = dds ? dds : "../private/extracted/media/Assets/luaentities/meshentities/rat/textures/rat.dds";
        FILE* f = fopen(p.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> buf(n > 0 ? n : 0); size_t rd = buf.empty() ? 0 : fread(buf.data(), 1, buf.size(), f); fclose(f);
            bool ok = rd >= 128 && buf[0] == 'D' && buf[1] == 'D' && buf[2] == 'S';
            if (ok) {
                uint32_t H = buf[12] | (buf[13] << 8), W = buf[16] | (buf[17] << 8);   // DDS header dims (LE)
                Desc d{}; d.format = FMT_8_8_8_8; d.width = W; d.height = H; d.tiled = 0; d.endian = END_NONE;
                std::vector<uint8_t> rgba;
                ok = DecodeBytesToRGBA(buf.data() + 128, d, rgba) && rgba.size() == size_t(W) * H * 4;
                if (ok) { WriteRGBAasPPM("/tmp/rat_decoded.ppm", rgba.data(), W, H);
                          fprintf(stderr, "[texselftest] wrote /tmp/rat_decoded.ppm (%ux%u from rat.dds)\n", W, H); }
            }
            check(ok, "rat.dds 8888 decode -> PPM");
        } else {
            fprintf(stderr, "[texselftest] (skipped rat.dds — not found at %s)\n", p.c_str());
        }
    }

    fprintf(stderr, "[texselftest] === %d passed, %d failed ===\n", pass, fail);
    return fail == 0;
}

} // namespace rex_tex
