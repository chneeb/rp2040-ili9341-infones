#!/bin/sh
#
# Configure and build Circle for the Waveshare GamePi20 (RP Zero, ST7789), then
# stash its libraries under software/infones/circle/libs/gamepi20/ so the app can
# link them without depending on the submodule's current configuration. That is
# what lets the GamePi20 and MHS35 builds coexist - see configure-mhs35.sh and
# software/infones/circle/Makefile.
#
# The three PWM audio options below have to be passed to Circle's configure,
# because they live in circle/include/circle/sysconfig.h, inside the submodule.
# configure writes them to circle/Config.mk, which Circle gitignores - so this
# script is the only record of them. Reconfiguring Circle by hand loses the
# audio settings, and the only symptom is silence.
#
# WARNING: the three options belong together. USE_PWM_AUDIO_ON_ZERO on its own
# puts PWM audio on GPIO 12 and 13, which on this board are the Up and Right
# buttons: outputs wired to switches that short to ground when pressed. The
# other two move the output to GPIO 18, where the earphone jack and the speaker
# are, and to GPIO 19, which is not connected here - so the sound is mono.
#
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
STASH="$ROOT/software/infones/circle/libs/gamepi20"

cd "$ROOT/circle"

./configure -r 1 -f \
	-d USE_PWM_AUDIO_ON_ZERO \
	-d USE_GPIO18_FOR_LEFT_PWM_ON_ZERO \
	-d USE_GPIO19_FOR_RIGHT_PWM_ON_ZERO


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
echo "Circle (GamePi20) is built and stashed."
echo "Now run 'make' in software/infones/circle to build build-gamepi20/kernel.img."
