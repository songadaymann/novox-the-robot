#!/bin/sh
# Copy the WebAssembly engine + JS wrapper into web/nvs/.
# Rebuild the wasm first (see bindings/js) if the engine changed.
set -e
cd "$(dirname "$0")"
mkdir -p nvs
for f in index.js nvs_wasm.js nvs_wasm.wasm; do
  cp "../bindings/js/$f" "nvs/$f"
done
echo "Copied engine into web/nvs/ ($(du -h nvs/nvs_wasm.wasm | cut -f1) wasm)"
