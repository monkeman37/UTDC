#!/usr/bin/env bash
# Builds the Dreamcast runner (1ST_READ.BIN) and boot sector (IP.BIN).
# Requires the KallistiOS toolchain (toolchain/build-toolchain.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KOS_BASE=${KOS_BASE:-/opt/toolchains/dc/kos}
set +u; source "$KOS_BASE/environ.sh"; set -u
BUILD="$ROOT/runner/build-dc"
cmake -S "$ROOT/runner" -B "$BUILD" -G Ninja -Wno-dev \
  -DPLATFORM=dreamcast -DCMAKE_TOOLCHAIN_FILE="$KOS_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LEGACY_GL=OFF -DENABLE_MODERN_GL=OFF -DENABLE_PHYSICS=OFF -DENABLE_FMOD=OFF \
  -DVIDEO_BACKEND=none -DENABLE_WAD14=OFF -DENABLE_WAD17=OFF "$@"
ninja -C "$BUILD"
sh-elf-objcopy -R .stack -O binary "$BUILD/butterscotch.elf" "$BUILD/1ST_READ.BIN"
MAKEIP="$KOS_BASE/utils/makeip/src/makeip"
[ -x "$MAKEIP" ] || make -C "$KOS_BASE/utils/makeip/src"
rm -f "$BUILD/IP.BIN"
"$MAKEIP" -g "UNDERTALE DC (UTDC)" -c "UTDC" -n "T-UTDC1" -e "V0.120" -i "GD-ROM1/1" -p "E000F10" -a "JUE" "$BUILD/IP.BIN" >/dev/null
ls -l "$BUILD/1ST_READ.BIN" "$BUILD/IP.BIN"
