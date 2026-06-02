// Variant A renderer — GLSL -> SPIR-V compiler (links libshaderc; the dev headers aren't installed, so the
// stable shaderc C API is declared inline). Compiles the title's pixel shaders (ported from the .updb HLSL
// to GLSL) for the PM4->Vulkan renderer. Usage: shadercc <in.frag|in.vert> <out.spv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// --- minimal shaderc C API (from libshaderc_shared.so.1) ---------------------------------------------
extern "C" {
typedef struct shaderc_compiler*  shaderc_compiler_t;
typedef struct shaderc_compilation_result* shaderc_compilation_result_t;
typedef struct shaderc_compile_options* shaderc_compile_options_t;
shaderc_compiler_t shaderc_compiler_initialize(void);
shaderc_compile_options_t shaderc_compile_options_initialize(void);
shaderc_compilation_result_t shaderc_compile_into_spv(
    shaderc_compiler_t, const char* source_text, size_t source_size,
    int shader_kind, const char* input_file_name, const char* entry_point_name,
    const shaderc_compile_options_t additional_options);
size_t      shaderc_result_get_length(const shaderc_compilation_result_t);
const char* shaderc_result_get_bytes(const shaderc_compilation_result_t);
int         shaderc_result_get_compilation_status(const shaderc_compilation_result_t);
const char* shaderc_result_get_error_message(const shaderc_compilation_result_t);
}
// shaderc_shader_kind: vertex=0, fragment=1 (we pick by file extension)
enum { KIND_VERTEX = 0, KIND_FRAGMENT = 1 };

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <in.vert|in.frag> <out.spv>\n", argv[0]); return 2; }
    const char* in = argv[1]; const char* out = argv[2];
    int kind = strstr(in, ".vert") ? KIND_VERTEX : KIND_FRAGMENT;

    FILE* f = fopen(in, "rb");
    if (!f) { perror("open in"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> src(n);
    if (fread(src.data(), 1, n, f) != (size_t)n) { perror("read"); return 1; }
    fclose(f);

    shaderc_compiler_t c = shaderc_compiler_initialize();
    shaderc_compilation_result_t r = shaderc_compile_into_spv(
        c, src.data(), src.size(), kind, in, "main", nullptr);
    if (shaderc_result_get_compilation_status(r) != 0) {
        fprintf(stderr, "[shadercc] %s FAILED:\n%s\n", in, shaderc_result_get_error_message(r));
        return 1;
    }
    size_t len = shaderc_result_get_length(r);
    FILE* o = fopen(out, "wb");
    if (!o) { perror("open out"); return 1; }
    fwrite(shaderc_result_get_bytes(r), 1, len, o);
    fclose(o);
    fprintf(stderr, "[shadercc] %s -> %s (%zu bytes SPIR-V, %zu words)\n", in, out, len, len / 4);
    return 0;
}
