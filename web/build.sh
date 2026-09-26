#!/bin/sh
# Copy the WebAssembly engine + JS wrapper into web/novox/.
# Rebuild the wasm first (see bindings/js) if the engine changed.
set -e
cd "$(dirname "$0")"
mkdir -p novox
for f in index.js novox_wasm.js novox_wasm.wasm; do
  cp "../bindings/js/$f" "novox/$f"
done
echo "Copied engine into web/novox/ ($(du -h novox/novox_wasm.wasm | cut -f1) wasm)"
