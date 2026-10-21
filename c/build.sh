#!/bin/sh
# Builds opentvp.
#   ./build.sh          native binary (gcc or cc, otherwise zig) -> build/opentvp (build/opentvp.exe on Windows)
#   ./build.sh all      + Windows, static Linux, universal Mac (requires: pip install ziglang)
# Python: PYTHON variable, otherwise python3, otherwise python (on Windows, python3 is often the Store shortcut).
set -e
cd "$(dirname "$0")"
VERSION=$(cat ../VERSION 2>/dev/null || echo dev)
D=third_party/libdeflate/lib
SRC="opentvp.c $D/deflate_compress.c $D/deflate_decompress.c $D/zlib_compress.c $D/zlib_decompress.c $D/adler32.c $D/crc32.c $D/utils.c $D/x86/cpu_features.c $D/arm/cpu_features.c"
FLAGS="-O3 -Ithird_party/libdeflate -DOPENTVP_VERSION=\"$VERSION\""
PY=${PYTHON:-$(python3 -c '' 2>/dev/null && echo python3 || echo python)}
ZIG="${ZIG:-$PY -m ziglang} cc"
mkdir -p build
CC_NATIVE=$(command -v gcc || command -v cc || true)
if [ -n "$CC_NATIVE" ]; then
    $CC_NATIVE $FLAGS -o build/opentvp $SRC -lm -lpthread
else
    case "$(uname -s)" in
        # -mevex512: otherwise zig 0.16 (latest available on Windows) rejects libdeflate 1.22's AVX-512 code
        MINGW*|MSYS*|CYGWIN*) $ZIG -target x86_64-windows-gnu $FLAGS -mevex512 -o build/opentvp.exe $SRC -lshell32 ;;
        *) $ZIG $FLAGS -o build/opentvp $SRC -lm -lpthread ;;
    esac
fi
if [ "${1:-}" = "all" ]; then
    $ZIG -target x86_64-windows-gnu $FLAGS -s -o build/opentvp.exe $SRC -lshell32
    $ZIG -target x86_64-linux-musl $FLAGS -s -static -o build/opentvp-linux $SRC
    $ZIG -target aarch64-macos $FLAGS -o build/opentvp-arm64 $SRC
    $ZIG -target x86_64-macos $FLAGS -o build/opentvp-x86_64 $SRC
    $PY make_universal.py build/opentvp-macos build/opentvp-x86_64 build/opentvp-arm64
    rm -f build/opentvp-arm64 build/opentvp-x86_64
fi
ls -la build
