#
# Makefile
#
# InfoNES on the Waveshare GamePi20, bare metal on a Raspberry Pi via Circle.
#
# The emulator core and the mappers live one directory up and are shared with
# the pico-sdk build, unchanged. Only the platform layer is here, so that
# merging from upstream stays possible.
#
# Build Circle first, from the repository root:
#
#	./configure-gamepi20.sh
#
# then run make in this directory.
#

CIRCLEHOME = ../../../circle
INFONES    = ..

# Panel: `make MHS35=1` builds for the goodtft MHS35 (ILI9486, 480x320) on a
# Pi 2/3; the default is the GamePi20 (ST7789, 320x240) on a Pi Zero. Only the
# selected driver is compiled, so each panel's DisplayConfig branch is the only
# one that has to resolve.
ifdef MHS35
DEFINE	+= -DPANEL_MHS35
PANEL_OBJ = ILI9486DMADisplay.o
else
PANEL_OBJ = ST7789DMADisplay.o
endif

OBJS	= main.o kernel.o $(PANEL_OBJ) RomMenu.o InfoNES_System_Circle.o

# Emulator core, shared with the pico build, untouched. The 137 mappers are
# not listed: InfoNES_Mapper.cpp #includes them all, so they are not separate
# translation units. This matches what CMakeLists.txt does for the pico build.
OBJS	+= $(INFONES)/InfoNES.o $(INFONES)/K6502.o $(INFONES)/InfoNES_pAPU.o
OBJS	+= $(INFONES)/InfoNES_Mapper.o

# The core is not quite platform free: it wraps hot functions in the pico-sdk's
# __not_in_flash_func(). PicoCompat.h defines those away, and is forced into
# every translation unit rather than editing the shared sources.
DEFINE	+= -include $(shell pwd)/PicoCompat.h

# Only these two reach InfoNES.cpp itself; the rest of the pico build's
# definitions belong to its own platform layer, which is not built here.
DEFINE	+= -DNES_FIRST_SCANLINE=0 -DNES_LAST_SCANLINE=239

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	  $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/libcircle.a

INCLUDE	+= -I . -I stdshim -I $(INFONES)

# An undefined name in #if is silently 0, which once left the scaling compiled
# out while a 320 wide window was still being set. Make that a warning.
DEFINE	+= -Wundef

include $(CIRCLEHOME)/app/Rules.mk

-include $(DEPS)

clean_all:
	@rm -f *.o *.d $(INFONES)/*.o $(INFONES)/*.d \
	       $(INFONES)/mapper/*.o $(INFONES)/mapper/*.d
