#!/bin/sh
# Compile tvpp.
#   ./build.sh          binaire natif (gcc ou cc) -> build/tvpp
#   ./build.sh all      + Windows, Linux statique, Mac universel (nécessite : pip install ziglang)
set -e
cd "$(dirname "$0")"
VERSION=$(cat ../VERSION 2>/dev/null || echo dev)
D=third_party/libdeflate/lib
SRC="tvpp.c $D/deflate_compress.c $D/deflate_decompress.c $D/zlib_compress.c $D/zlib_decompress.c $D/adler32.c $D/crc32.c $D/utils.c $D/x86/cpu_features.c $D/arm/cpu_features.c"
FLAGS="-O3 -Ithird_party/libdeflate -DTVPP_VERSION=\"$VERSION\""
ZIG="${ZIG:-python3 -m ziglang} cc"
mkdir -p build
CC_NATIVE=$(command -v gcc || command -v cc)
$CC_NATIVE $FLAGS -o build/tvpp $SRC -lm -lpthread
if [ "${1:-}" = "all" ]; then
    $ZIG -target x86_64-windows-gnu $FLAGS -s -o build/tvpp.exe $SRC -lshell32
    $ZIG -target x86_64-linux-musl $FLAGS -s -static -o build/tvpp-linux $SRC
    $ZIG -target aarch64-macos $FLAGS -o build/tvpp-arm64 $SRC
    $ZIG -target x86_64-macos $FLAGS -o build/tvpp-x86_64 $SRC
    python3 make_universal.py build/tvpp-macos build/tvpp-x86_64 build/tvpp-arm64
    rm -f build/tvpp-arm64 build/tvpp-x86_64
fi
ls -la build
