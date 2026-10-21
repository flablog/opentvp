#!/bin/sh
# Prépare les archives de release dans dist/ (après ./build.sh all).
set -e
cd "$(dirname "$0")/.."
V=$(cat VERSION)
rm -rf dist && mkdir -p dist
for p in windows:tvpp.exe macos:tvpp-macos linux:tvpp-linux; do
    plat=${p%%:*}; bin=${p#*:}
    d=dist/tvpp-$V-$plat
    mkdir -p $d
    cp c/build/$bin $d/
    cp docs/UTILISATION.md $d/LISEZMOI.md
    cp docs/EXPORT_CALQUES.md $d/
    cp c/third_party/libdeflate/COPYING $d/LICENCE-libdeflate.txt
    (cd dist && python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1],'zip','.',sys.argv[1])" tvpp-$V-$plat)
    rm -rf $d
done
d=dist/tvpp-python-$V
mkdir -p $d && cp python/*.py python/requirements.txt python/README.md VERSION docs/EXPORT_CALQUES.md $d/
(cd dist && python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1],'zip','.',sys.argv[1])" tvpp-python-$V) && rm -rf $d
ls -la dist
