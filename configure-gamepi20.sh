#!/bin/sh
#
# Configure and build Circle for the Waveshare GamePi20.
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

cd "$(dirname "$0")/circle"

./configure -r 1 -f \
	-d USE_PWM_AUDIO_ON_ZERO \
	-d USE_GPIO18_FOR_LEFT_PWM_ON_ZERO \
	-d USE_GPIO19_FOR_RIGHT_PWM_ON_ZERO

echo
echo "Building Circle ..."
./makeall -j"$(nproc)"

for addon in SDCard fatfs display; do
	echo "Building addon/$addon ..."
	make -C "addon/$addon" -j"$(nproc)"
done

echo
echo "Circle is built. Now run 'make' in the project root to build kernel.img."
