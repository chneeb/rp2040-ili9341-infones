#!/bin/sh
#
# Configure and build Circle for the goodtft MHS35 (Pi 2/3, ILI9486), then stash
# its libraries under software/infones/circle/libs/mhs35/ so the app can link
# them without depending on the submodule's current configuration. This is the
# Pi 2/3 counterpart to configure-gamepi20.sh; the two stashes let both builds
# coexist - see software/infones/circle/Makefile.
#
# RASPPI=2 boots on both the Pi 2B and Pi 3B (shared peripheral base) and
# produces kernel7.img, which the firmware auto-selects.
#
# NO *_ON_ZERO PWM flags here, unlike the GamePi20: on a Pi 2/3 the sound routes
# to the board's real 3.5mm jack (Circle picks the analog audio pins on a Pi 3
# automatically when those symbols are absent), so passing them would be wrong.
#
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
STASH="$ROOT/software/infones/circle/libs/mhs35"

cd "$ROOT/circle"

./configure -r 2 -f


echo
echo "Building Circle ..."
# A reconfigure does not invalidate the existing objects - Make cannot see that
# RASPPI or the sysconfig flags changed - so an incremental build would silently
# keep the previous target's objects. Clean first, always.
./makeall clean
./makeall -j"$(nproc)"

for addon in SDCard fatfs display; do
	echo "Building addon/$addon ..."
	make -C "addon/$addon" clean
	make -C "addon/$addon" -j"$(nproc)"
done

echo
echo "Stashing libraries -> $STASH"
mkdir -p "$STASH"
for lib in lib/libcircle.a lib/sound/libsound.a lib/usb/libusb.a \
	   lib/usb/gadget/libusbgadget.a lib/input/libinput.a lib/fs/libfs.a \
	   addon/fatfs/libfatfs.a addon/SDCard/libsdcard.a; do
	cp "$lib" "$STASH/"
done

echo
echo "Circle (MHS35) is built and stashed."
echo "Now run 'make MHS35=1' in software/infones/circle to build build-mhs35/kernel7.img."
