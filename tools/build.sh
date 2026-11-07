#!/usr/bin/env bash
# Builds the firmware: fast incremental builds, one persistent cache folder per variant.
#   tools/build.sh            both variants, in parallel
#   tools/build.sh batt       board with a battery      -> build/fwemoji
#   tools/build.sh nobatt     board without a battery   -> build/fwemoji_nobatt  (-DBATTERY_INSTALLED=0)
# Add "clean" as a second word to force a full rebuild of that variant (needed after editing only build_opt.h).
# Flash with: arduino-cli upload -p COMx --fqbn "$FQBN" --input-dir build/<folder> firmware/TobiaTalk
set -u
CLI="/f/Program Files/ArduinoIDE/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=4M,FlashMode=qio,PartitionScheme=min_spiffs,PSRAM=opi"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
which_one="${1:-both}"
clean="${2:-}"

build() {   # name, extra flags
  local name="$1" flags="$2" out="$ROOT/build/fw$1" cache="$ROOT/build/cache_$1"
  [ "$clean" = "clean" ] && rm -rf "$cache"
  "$CLI" compile --fqbn "$FQBN" --build-path "$cache" --output-dir "$out" \
    --build-property "compiler.cpp.extra_flags=$flags" "$ROOT/firmware/TobiaTalk" 2>&1 \
    | grep -E "error|Sketch uses" | sed "s/^/[$name] /"
}

case "$which_one" in
  batt)   build emoji "" ;;
  nobatt) build emoji_nobatt "-DBATTERY_INSTALLED=0" ;;
  both)   build emoji "" & build emoji_nobatt "-DBATTERY_INSTALLED=0" & wait ;;
  *) echo "usage: tools/build.sh [batt|nobatt|both] [clean]"; exit 1 ;;
esac
