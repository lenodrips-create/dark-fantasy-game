#!/bin/sh
# Build the C game to WebAssembly. Output goes to ../game (served by GitHub Pages).
set -e
cd "$(dirname "$0")"
RAYLIB="${RAYLIB:-$HOME/Developer/raylib/src}"
export EMSDK_PYTHON="${EMSDK_PYTHON:-/Applications/Blender.app/Contents/Resources/4.5/python/bin/python3.11}"
. "$HOME/Developer/emsdk/emsdk_env.sh" >/dev/null 2>&1
mkdir -p ../game
emcc src/main.c -o ../game/index.html -Os -Wall \
  -I"$RAYLIB" "$RAYLIB/libraylib.a" -DPLATFORM_WEB \
  -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1mb \
  --preload-file assets --shell-file shell.html
echo "Built ../game/index.html"
