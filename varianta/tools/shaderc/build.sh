#!/bin/sh
# Build the GLSL->SPIR-V compiler (links the installed libshaderc; no dev package needed) and compile
# every ported shader in ported/ to out/<name>.spv. Run from this dir: ./build.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
LIBSHADERC="$(ldconfig -p | sed -n 's/.*=> //p' | grep -m1 libshaderc_shared)"
[ -n "$LIBSHADERC" ] || { echo "libshaderc_shared not found (install shaderc)"; exit 1; }
g++ -O2 -o "$HERE/shadercc" "$HERE/compile.cpp" "$LIBSHADERC"
mkdir -p "$HERE/out"
n=0; for f in "$HERE"/ported/*.frag "$HERE"/ported/*.vert; do
    [ -e "$f" ] || continue
    "$HERE/shadercc" "$f" "$HERE/out/$(basename "$f").spv" && n=$((n+1))
done
echo "compiled $n shaders -> $HERE/out/"
