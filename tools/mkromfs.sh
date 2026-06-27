#!/usr/bin/env bash
# Build a FAT32 image from a local directory, suitable for flashing into the
# RP2350-PiZero's XIP flash at FLASHFS_BASE_ADDR (0x10200000 for GAMEPI20).
#
# The infoNES GAMEPI20 firmware mounts this image as FatFs drive 1 when no SD
# card is present and shows its contents in the menu. Saves still go to
# dedicated flash sectors below NES_FILE_ADDR (independent of this image).
#
# Usage:
#   tools/mkromfs.sh <source-dir> <output.img> [size_mb=14]
#
# Then flash with picotool (device in BOOTSEL):
#   picotool load <output.img> -t bin -o 0x10200000
#
# Dependencies: dosfstools (mkfs.fat) + mtools (mcopy, mmd).
#   Debian/Ubuntu: sudo apt install dosfstools mtools
#   macOS:         brew install dosfstools mtools

set -euo pipefail

usage() {
    echo "Usage: $0 <source-dir> <output.img> [size_mb=14]" >&2
    exit 1
}

[[ $# -ge 2 && $# -le 3 ]] || usage

SRC=$1
OUT=$2
SIZE_MB=${3:-14}

for tool in mkfs.fat mcopy mmd; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: missing '$tool' — install dosfstools and mtools" >&2
        exit 1
    fi
done

[[ -d "$SRC" ]] || { echo "error: '$SRC' is not a directory" >&2; exit 1; }

if ! [[ "$SIZE_MB" =~ ^[0-9]+$ ]] || (( SIZE_MB < 1 || SIZE_MB > 16 )); then
    echo "error: size_mb must be an integer 1..16 (got '$SIZE_MB')" >&2
    exit 1
fi

SIZE_KB=$(( SIZE_MB * 1024 ))

echo ">> Creating ${SIZE_MB} MB FAT32 image at $OUT"
# -F 32: force FAT32. -C: create image of given size in KB. -n: volume label.
mkfs.fat -F 32 -C "$OUT" "$SIZE_KB" -n "INFONES" >/dev/null

echo ">> Copying .nes files from $SRC (top-level only, case-insensitive)"
# nocaseglob makes the *.nes pattern match .NES, .Nes, etc. Subdirectories and
# non-ROM files (.txt, screenshots, archives) are deliberately skipped.
shopt -s nullglob nocaseglob
count=0
skipped=0
for item in "$SRC"/*; do
    name=$(basename "$item")
    if [[ -d "$item" ]]; then
        skipped=$(( skipped + 1 ))
        continue
    fi
    if [[ ! -f "$item" ]]; then
        continue
    fi
    # Case-insensitive .nes extension check
    shopt -s nocasematch
    if [[ "$name" != *.nes ]]; then
        skipped=$(( skipped + 1 ))
        shopt -u nocasematch
        continue
    fi
    shopt -u nocasematch
    mcopy -i "$OUT" "$item" "::/$name"
    count=$(( count + 1 ))
done
shopt -u nocaseglob

if (( count == 0 )); then
    echo "warning: no .nes files found in $SRC" >&2
fi
echo ">> Copied $count .nes file(s)"
if (( skipped > 0 )); then
    echo ">> Skipped $skipped non-.nes entries (subdirectories, other file types)"
fi

echo ">> Image contents (top level):"
mdir -i "$OUT" ::/ | sed 's/^/   /'

cat <<EOF

>> Done. Flash to the RP2350-PiZero at FLASHFS_BASE_ADDR (0x10200000):
     picotool load $OUT -t bin -o 0x10200000
   With the board in BOOTSEL (hold the BOOT key while connecting USB).

   The firmware will mount this as FatFs drive 1 whenever no SD card is
   present and serve ROMs from it in the menu.
EOF
