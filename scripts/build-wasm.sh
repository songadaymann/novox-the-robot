#!/usr/bin/env bash
# Build the WebAssembly engine and copy it into the npm package (bindings/js).
# Needs Emscripten (brew install emscripten, or emsdk).
set -euo pipefail
cd "$(dirname "$0")/.."
# Homebrew's emcc is a Python script and needs Python >= 3.10.
export EMSDK_PYTHON="${EMSDK_PYTHON:-$(command -v python3)}"
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release -DNVS_BUILD_CLI=OFF -DNVS_BUILD_TESTS=OFF \
  -DPython3_EXECUTABLE="$EMSDK_PYTHON"
cmake --build build-wasm -j --target nvs_wasm
cp build-wasm/nvs_wasm.js build-wasm/nvs_wasm.wasm bindings/js/
cp LICENSE bindings/js/
echo "bindings/js ready: $(du -h bindings/js/nvs_wasm.wasm | cut -f1) wasm"
