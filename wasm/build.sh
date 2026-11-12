#!/bin/sh
# ブラウザ用の WASM（docs/tb.js, docs/tb.wasm）。EMSDK で emsdk の場所を変えられる
set -e
cd "$(dirname "$0")/.."
EMSDK="${EMSDK:-/c/prog/emsdk/emsdk}"
EMCC="$EMSDK/upstream/emscripten/emcc.exe"
[ -x "$EMCC" ] || EMCC="$EMSDK/upstream/emscripten/emcc"
"$EMCC" -std=c++17 -O3 -msimd128 -DNDEBUG -I. -Ithird_party -Ithird_party/nlohmann \
  -s MODULARIZE=1 -s EXPORT_NAME=createTB -s ALLOW_MEMORY_GROWTH=1 -s STACK_SIZE=4194304 \
  -s EXPORTED_RUNTIME_METHODS='["HEAPU8","UTF8ToString","stringToUTF8","lengthBytesUTF8"]' -s EXPORTED_FUNCTIONS='["_malloc","_free"]' \
  wasm/tb_wasm.cpp -o docs/tb.js
cp data/tobus.txt docs/tobus.txt
cp data/admin_tokyo.txt docs/admin_tokyo.txt
cp fonts/NotoSansJP-Bold-subset.ttf docs/font.ttf
echo "built docs/tb.js"
