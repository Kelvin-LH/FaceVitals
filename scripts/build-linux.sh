#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${FV_BUILD_DIR:-$root/build/linux}"
ort="${ONNXRUNTIME_ROOT:-$root/external/onnxruntime-linux-x64-1.23.2}"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_ROOT="$ort"
cmake --build "$build" --parallel
ctest --test-dir "$build" --output-on-failure
