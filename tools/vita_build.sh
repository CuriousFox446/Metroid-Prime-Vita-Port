#!/bin/bash
# Configures and builds the PS Vita VPKs, the game's and Metroid Prime Demo's (see docs/VITA.md).
#   tools/vita_build.sh [configure|build|all] [extra cmake -D args...]
set -e
: "${VITASDK:=$HOME/vitasdk}"
export VITASDK PATH="$VITASDK/bin:$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."
step="${1:-all}"
shift || true
build=build/vita
if [ "$step" = configure ] || [ "$step" = all ]; then
  cmake -S . -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMP_USE_CCACHE=OFF \
    "$@"
fi
if [ "$step" = build ] || [ "$step" = all ]; then
  cmake --build "$build" --target metroid_prime_port.vpk metroid_prime_port_demo.vpk -- -k 0
fi
