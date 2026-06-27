#!/usr/bin/env bash
# Build a POSIX ustar tarball of .nes ROMs, suitable for flashing into the
# RP2040/RP2350 single-ROM region at NES_FILE_ADDR = 0x10080000.
#
# The infoNES firmware's RomSelector (software/infones/rom_selector.h) tries
# to interpret the bytes at NES_FILE_ADDR as a single iNES ROM first (looks
# for the "NES\x1A" magic). If that fails, it parses the bytes as a ustar
# archive (software/infones/tar.cpp) and lists each ROM as a separate entry.
# In-game switching: hold SELECT and tap LEFT/RIGHT to cycle ROMs.
#
# Usage:
#   tools/mkromtar.sh <source-dir> <output.tar>
#
# Then flash to NES_FILE_ADDR (device in BOOTSEL):
#   picotool load -F <output.tar> -t bin -o 0x10080000 --family absolute
#   picotool reboot
#
# Notes:
# - Only top-level *.nes files are included (case-insensitive). Subdirectories
#   and non-.nes files are skipped.
# - parseTAR requires the POSIX ustar magic at offset 257 of each header.
#   GNU tar's default --format=gnu adds PaxHeader records the parser rejects;
#   we force --format=ustar.
# - The NES_FILE_ADDR region is 1 MB (0x10080000 → 0x10180000). The script
#   warns if the resulting tar exceeds that.

set -euo pipefail

usage() {
    echo "Usage: $0 <source-dir> <output.tar>" >&2
    exit 1
}

[[ $# -eq 2 ]] || usage

SRC=$1
OUT=$2

command -v tar >/dev/null 2>&1 || { echo "error: tar not found" >&2; exit 1; }

[[ -d "$SRC" ]] || { echo "error: '$SRC' is not a directory" >&2; exit 1; }

# Gather top-level .nes files, case-insensitive. Sort so the in-tar order is
# deterministic (so SELECT+LEFT/RIGHT cycles in the same order across builds).
shopt -s nullglob nocaseglob
FILES=()
for item in "$SRC"/*.nes; do
    [[ -f "$item" ]] && FILES+=("$(basename "$item")")
done
shopt -u nocaseglob

if (( ${#FILES[@]} == 0 )); then
    echo "error: no .nes files found in $SRC" >&2
    exit 1
fi

# Sort alphabetically by filename.
IFS=$'\n' read -r -d '' -a SORTED < <(printf '%s\n' "${FILES[@]}" | sort && printf '\0') || true

# Resolve $OUT to an absolute path BEFORE invoking tar — we cd into $SRC and
# would otherwise create the archive inside the source directory.
if [[ "$OUT" != /* ]]; then
    OUT="$PWD/$OUT"
fi

echo ">> Bundling ${#SORTED[@]} .nes file(s) from $SRC"
( cd "$SRC" && tar --format=ustar -cf "$OUT" "${SORTED[@]}" )

# wc -c is universally available; sidesteps GNU vs. BSD stat differences.
TAR_BYTES=$(wc -c < "$OUT")
LIMIT=$((1024 * 1024))
echo ">> Output: $OUT ($TAR_BYTES bytes)"

# Sanity-check the ustar magic actually got written at offset 257.
MAGIC=$(dd if="$OUT" bs=1 skip=257 count=5 status=none)
if [[ "$MAGIC" != "ustar" ]]; then
    echo "error: ustar magic missing at offset 257 (your tar built a non-ustar archive)" >&2
    exit 1
fi
echo ">> ustar magic verified at offset 257"

if (( TAR_BYTES > LIMIT )); then
    echo ""
    echo "WARNING: tar is $TAR_BYTES bytes; the NES_FILE_ADDR region is only" >&2
    echo "         $LIMIT bytes (1 MB at 0x10080000 → 0x10180000). Flashing" >&2
    echo "         a larger image will overflow into other flash regions." >&2
fi

cat <<EOF

>> Flash to the device (BOOTSEL mode):
     picotool load -F $OUT -t bin -o 0x10080000 --family absolute
     picotool reboot

>> First ROM in alphabetical order plays at boot.
>> In-game: hold SELECT and tap LEFT/RIGHT to cycle through ROMs.
EOF
