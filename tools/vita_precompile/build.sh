#!/usr/bin/env bash
# Builds precompile_sources (PS Vita's program generator, on the PC) with the host's g++, from the
# Vita build's own dependency trees: configure build/vita first (tools/vita_build.sh).
#
#   tools/vita_precompile/build.sh [out dir, default build/vita-precompile]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="$ROOT/build/vita/_deps"
OUT="${1:-$ROOT/build/vita-precompile}"
AURORA="$ROOT/extern/aurora"
mkdir -p "$OUT"

# As the Vita build compiles vita_tev.cpp (its defines and include paths), fmt header-only.
FLAGS=(
  -std=gnu++20 -O2 -DNDEBUG -DFMT_HEADER_ONLY=1
  -DAURORA -DAURORA_ENABLE_GX -DAURORA_NULL_GPU -DAURORA_VITA_GL -DTARGET_PC -DWEBGPU_DAWN
  "-DIMGUI_USER_CONFIG=\"aurora/imgui_config.h\""
  -I"$AURORA/lib" -I"$AURORA/include"
  -I"$AURORA/platforms/vita/webgpu_null/include" -I"$AURORA/platforms/vita/profile"
  -I"$DEPS/abseil-cpp-src" -I"$DEPS/fmt-src/include" -I"$DEPS/imgui-src" -I"$DEPS/sdl-src/include"
  -I"$DEPS/sdl-build/include-revision" -I"$DEPS/tracy-src/public" -I"$DEPS/xxhash-src"
)

gcc -O2 -c "$DEPS/xxhash-src/xxhash.c" -o "$OUT/xxhash.o"
# GXState holds abseil hash maps: their empty table's sentinel and the hash's seed.
ABSL=()
for src in container/internal/raw_hash_set hash/internal/hash hash/internal/city hash/internal/low_level_hash; do
  g++ "${FLAGS[@]}" -c "$DEPS/abseil-cpp-src/absl/$src.cc" -o "$OUT/$(basename "$src").o"
  ABSL+=("$OUT/$(basename "$src").o")
done
g++ "${FLAGS[@]}" -c "$AURORA/lib/gx/vita_tev.cpp" -o "$OUT/vita_tev.o"
g++ "${FLAGS[@]}" -c "$ROOT/tools/vita_precompile/precompile_sources.cpp" -o "$OUT/precompile_sources.o"
g++ "$OUT/precompile_sources.o" "$OUT/vita_tev.o" "${ABSL[@]}" "$OUT/xxhash.o" -o "$OUT/precompile_sources"
echo "$OUT/precompile_sources"
