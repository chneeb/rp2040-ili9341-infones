#!/bin/sh
#
# flash-probe-sd.sh
#
# Prepare an SD card to boot the MHS35 probe on a Pi 2B / 3B.
#
# The Pi 3B boot ROM reads only an MBR (msdos) partition table with a FAT16/32
# partition - a GPT card (which is what Linux formatting tools tend to produce)
# leaves it dead at power-on. This wipes the given card, lays down MBR + one
# FAT32 partition, and copies the five boot files.
#
# Usage:
#   sudo ./flash-probe-sd.sh /dev/sdX
#
# It REFUSES anything that is not a removable USB disk, so it cannot be pointed
# at an internal drive by accident. Pass -y to skip the confirmation prompt.
#
set -e

YES=0
if [ "$1" = "-y" ]; then YES=1; shift; fi

DEV="$1"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/../../../.." && pwd)
BOOT="$REPO/circle/boot"
KERNEL="$SCRIPT_DIR/kernel7.img"

die () { echo "error: $*" >&2; exit 1; }

# --- checks -----------------------------------------------------------------

[ -n "$DEV" ]     || die "no device given. Usage: sudo $0 /dev/sdX"
[ -b "$DEV" ]     || die "$DEV is not a block device"
[ "$(id -u)" = 0 ] || die "must run as root (use sudo)"

# The whole-disk device only, not a partition (/dev/sda, not /dev/sda1).
case "$DEV" in
	*[0-9]) die "$DEV looks like a partition - give the whole disk, e.g. ${DEV%[0-9]*}" ;;
esac

BASE=$(basename "$DEV")
RM=$(cat "/sys/block/$BASE/removable" 2>/dev/null || echo 0)
TRAN=$(lsblk -dno TRAN "$DEV" 2>/dev/null || echo "")

[ "$RM" = "1" ]     || die "$DEV is not removable - refusing (is this an internal disk?)"
[ "$TRAN" = "usb" ] || die "$DEV is not a USB device (transport: '$TRAN') - refusing"

for f in "$BOOT/bootcode.bin" "$BOOT/start.elf" "$BOOT/fixup.dat" \
	 "$BOOT/config32.txt" "$KERNEL"; do
	[ -f "$f" ] || die "missing source file: $f"
done

# --- confirm ----------------------------------------------------------------

SIZE=$(lsblk -dno SIZE "$DEV")
MODEL=$(lsblk -dno MODEL "$DEV" 2>/dev/null || echo "")
echo "About to ERASE $DEV  ($SIZE, $MODEL) and write the MHS35 probe."
if [ "$YES" != "1" ]; then
	printf "Type YES to continue: "
	read ANSWER
	[ "$ANSWER" = "YES" ] || die "aborted"
fi

# --- do it ------------------------------------------------------------------

PART="${DEV}1"
# NVMe/mmc name partitions p1; SD-over-USB is sdX1, so the plain suffix is right,
# but handle the p-form too just in case.
[ -b "$PART" ] || PART="${DEV}p1"

echo "Unmounting any existing partitions ..."
for p in "$DEV"*; do umount "$p" 2>/dev/null || true; done

echo "Writing MBR + FAT32 partition ..."
parted -s "$DEV" mklabel msdos
parted -s "$DEV" mkpart primary fat32 1MiB 100%
# Settle so the kernel re-reads the new partition before mkfs.
sleep 1
[ -b "$PART" ] || PART="${DEV}p1"
mkfs.vfat -F 32 -n BOOT "$PART" >/dev/null

echo "Copying boot files ..."
MNT=$(mktemp -d)
mount "$PART" "$MNT"
cp "$BOOT/bootcode.bin" "$BOOT/start.elf" "$BOOT/fixup.dat" "$MNT/"
cp "$BOOT/config32.txt" "$MNT/config.txt"
cp "$KERNEL" "$MNT/kernel7.img"
sync
echo
echo "Card contents:"
ls -la "$MNT/"
umount "$MNT"
rmdir "$MNT"

echo
echo "Done. Put the card in the Pi and power on; watch the green ACT LED."
