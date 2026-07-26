#
# Makefile
#
# InfoNES on the Waveshare GamePi20, bare metal on a Raspberry Pi via Circle.
#
# The emulator core and the mappers live one directory up and are shared with
# the pico-sdk build, unchanged. Only the platform layer is here, so that
# merging from upstream stays possible.
#
# This is a dispatcher: it builds the selected target inside its own directory
# (build-gamepi20/ or build-mhs35/) using infones.mk, so the two coexist - the
# objects and the kernel image of one are never disturbed by building the other.
#
# Build Circle once per panel, from the repository root:
#
#	./configure-gamepi20.sh        # RP Zero, ST7789      -> libs/gamepi20/
#	./configure-mhs35.sh           # Pi 2/3, ILI9486      -> libs/mhs35/
#
# then, in this directory:
#
#	make                           # GamePi20  -> build-gamepi20/kernel.img
#	make MHS35=1                   # MHS35     -> build-mhs35/kernel7.img
#
# Once both libs/ stashes exist, switching between the two needs no Circle
# reconfigure and no clean - just the other make line.
#

INFONES_MK := $(CURDIR)/infones.mk

# The panel selects both the build directory and the chip: GamePi20 is an RP
# Zero (RASPPI=1, kernel.img), MHS35 is a Pi 2/3 (RASPPI=2, kernel7.img).
ifeq ($(strip $(MHS35)),1)
BUILDDIR := build-mhs35
RASPPI   := 2
else
BUILDDIR := build-gamepi20
RASPPI   := 1
endif

.PHONY: all clean clean-all

# RASPPI is passed on the sub-make command line so it overrides the submodule's
# Config.mk whatever that was last set to.
all:
	@mkdir -p $(BUILDDIR)
	@$(MAKE) --no-print-directory -C $(BUILDDIR) -f $(INFONES_MK) \
		$(if $(filter 1,$(MHS35)),MHS35=1) RASPPI=$(RASPPI)

# Remove just the selected target's build directory.
clean:
	@rm -rf $(BUILDDIR)

# Remove both.
clean-all:
	@rm -rf build-gamepi20 build-mhs35
