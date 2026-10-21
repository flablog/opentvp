#!/bin/sh
# Builds the release archives in dist/ (after ./build.sh all).
set -e
PY=${PYTHON:-$(python3 -c '' 2>/dev/null && echo python3 || echo python)}
cd "$(dirname "$0")/.."
V=$(cat VERSION)
rm -rf dist && mkdir -p dist
for p in windows:opentvp.exe macos:opentvp-macos linux:opentvp-linux; do
    plat=${p%%:*}; bin=${p#*:}
    d=dist/opentvp-$V-$plat
    mkdir -p $d
    cp c/build/$bin $d/
    cp docs/USAGE.md $d/README.md
    cp docs/LAYER_EXPORT.md $d/
    cp LICENSE $d/LICENSE.txt
    cp c/third_party/libdeflate/COPYING $d/LICENSE-libdeflate.txt
    [ $plat = linux ] && cp c/third_party/musl/COPYRIGHT $d/LICENSE-musl.txt
    (cd dist && $PY -c "import shutil,sys; shutil.make_archive(sys.argv[1],'zip','.',sys.argv[1])" opentvp-$V-$plat)
    rm -rf $d
done
d=dist/opentvp-python-$V
mkdir -p $d && cp python/*.py python/requirements.txt python/README.md VERSION docs/LAYER_EXPORT.md $d/
cp LICENSE $d/LICENSE.txt
(cd dist && $PY -c "import shutil,sys; shutil.make_archive(sys.argv[1],'zip','.',sys.argv[1])" opentvp-python-$V) && rm -rf $d
ls -la dist
