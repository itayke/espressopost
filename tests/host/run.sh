#!/usr/bin/env bash
# Convenience wrapper: configure (first time only), build, run.
# Use `./run.sh -s` to pass through Catch2's reporter flags etc. — anything
# after the first arg gets handed to the test binary.

set -euo pipefail
cd "$(dirname "$0")"

if [[ ! -d build ]]; then
    cmake -S . -B build -GNinja >/dev/null
fi
cmake --build build >/dev/null
./build/test_model "$@"
./build/test_cloud "$@"
