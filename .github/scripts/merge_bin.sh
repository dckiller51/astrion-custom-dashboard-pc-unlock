#!/usr/bin/env bash
# Merges an environment's bootloader, partitions and app into one image to flash at 0x0:
#   merge_bin.sh <env> <out.factory.bin>
set -euo pipefail
env="$1"; out="$2"
b=".pio/build/$env"
boot_app0=$(find ~/.platformio/packages/framework-arduinoespressif32 -name boot_app0.bin | head -1)
~/.platformio/penv/bin/python -m esptool --chip esp32s3 merge-bin -o "$out" \
  0x0 "$b/bootloader.bin" 0x8000 "$b/partitions.bin" 0xe000 "$boot_app0" 0x10000 "$b/firmware.bin"
