#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

# Keep the UI footer and the API's firmware version in sync for OTA detection.
export WHISLI_VERSION="0.002.$(date +%Y%m%d%H%M)"

# PlatformIO uruchamia scripts/embed_ui.py przed kompilacją. Ten etap buduje
# React, gzipuje zasoby panelu i dołącza je do pojedynczego firmware.bin.
platformio run -e waveshare_esp32s3 "$@"

build_dir="$project_dir/_build"
mkdir -p "$build_dir"
# firmware.bin — aktualizacja przez panel (OTA); firmware-full.bin — pierwsze wgranie przez USB
# od adresu 0x0 (bootloader, tablica partycji i aplikacja w jednym pliku).
cp "$project_dir/.pio/build/waveshare_esp32s3/firmware.bin" "$build_dir/firmware.bin"
cp "$project_dir/.pio/build/waveshare_esp32s3/firmware.factory.bin" "$build_dir/firmware-full.bin"
echo
echo "Firmware gotowy (wersja $WHISLI_VERSION):"
echo "  $build_dir/firmware.bin       aktualizacja przez panel"
echo "  $build_dir/firmware-full.bin  pierwsze wgranie przez USB (adres 0x0)"
