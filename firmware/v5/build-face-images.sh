#!/usr/bin/env bash
# Build both face looks of the v5 firmware and install them where the Windows
# companion's tray (Face style → Bot / Classic) flashes them from.
# The look is fixed per build (FACE_LOOK_BOT) — see macropad_v5.ino for why.
set -euo pipefail
cd "$(dirname "$0")/macropad_v5"
ACLI="${ARDUINO_CLI:-arduino-cli}"
FQBN="esp32:esp32:esp32:FlashFreq=40,UploadSpeed=115200,PartitionScheme=min_spiffs"
LIBS="${ARDUINO_LIBS:-$HOME/Documents/Arduino/libraries}"
OUT="${FACE_IMAGES_DIR:-$HOME/MacroPadDeck/firmware}"

"$ACLI" compile --fqbn "$FQBN" --libraries "$LIBS" --output-dir build-bot .
"$ACLI" compile --fqbn "$FQBN" --libraries "$LIBS" \
  --build-property "compiler.cpp.extra_flags=-DFACE_LOOK_BOT=0" --output-dir build-classic .

mkdir -p "$OUT"
cp build-bot/macropad_v5.ino.bin     "$OUT/macropad_v5_bot.bin"
cp build-classic/macropad_v5.ino.bin "$OUT/macropad_v5_classic.bin"
# Identical images would mean the -D flag never reached the compiler
if cmp -s "$OUT/macropad_v5_bot.bin" "$OUT/macropad_v5_classic.bin"; then
  echo "ERROR: bot and classic images are identical — FACE_LOOK_BOT not applied" >&2; exit 1
fi
md5sum "$OUT"/macropad_v5_*.bin
