#!/bin/sh
# regenerate reference vectors, build, run
set -e
cd "$(dirname "$0")/.."
python3 tests/ref_impl.py > tests/expected.h
cmake -S . -B build >/dev/null
cmake --build build >/dev/null
ctest --test-dir build --output-on-failure
