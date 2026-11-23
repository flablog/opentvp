#!/bin/sh
# Builds the native binary, then runs every corpus test (C and Python).
# Python: PYTHON variable, otherwise python3, otherwise python.
set -e
cd "$(dirname "$0")/.."
PY=${PYTHON:-$(python3 -c '' 2>/dev/null && echo python3 || echo python)}
./c/build.sh > /dev/null
$PY tests/check_layers.py --impl c
$PY tests/check_layers.py --impl python
$PY tests/check_camera.py --impl c
$PY tests/check_sound.py
