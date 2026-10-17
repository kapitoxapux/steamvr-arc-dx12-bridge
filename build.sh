#!/bin/sh
# Cross-compiles bin/openvr_api.dll (x64) with zig 0.13+: https://ziglang.org/download/
# Needs a clang-compatible compiler (naked functions, GNU inline asm), MSVC won't do.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
mkdir -p bin
"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 -shared -o bin/openvr_api.dll src/proxy.cpp \
    -ld3d11 -ld3d12 -ldxgi
rm -f bin/*.pdb bin/*.lib
sha256sum bin/openvr_api.dll
