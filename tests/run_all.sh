#!/bin/sh
# Compile le binaire natif puis lance tous les tests du corpus (C et Python).
set -e
cd "$(dirname "$0")/.."
./c/build.sh > /dev/null
python3 tests/check_layers.py --impl c
python3 tests/check_layers.py --impl python
python3 tests/check_camera.py --impl c
