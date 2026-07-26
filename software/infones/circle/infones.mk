#
# infones.mk
#
# Shared build recipe for both panels, run from a per-target build directory
# (build-gamepi20/ or build-mhs35/) by the dispatcher Makefile - never directly.
# Running one target in its own directory is what lets both images coexist: the
# objects and the kernel image never collide, and switching targets needs no
# clean.
#
# All paths here are relative to the build directory, which sits one level below
# the port directory (software/infones/circle/):
#
#	build-gamepi20/  ->  ..            the port layer (main.cpp, kernel.cpp)
#	                     ../..         the shared InfoNES core
#	                     ../../../../circle   the Circle submodule
#
# RASPPI is passed on the command line by the dispatcher, so it overrides the
# submodule's Config.mk (command-line variables win). That is what decouples the
# app build from however Circle was last configured: the headers are the same
# for either chip, only the libraries differ, and those come from a per-target
# stash (libs/<target>/) populated by the configure scripts.
#

CIRCLEHOME = ../../../../circle
INFONES    = ../..
PORT       = ..

ifeq ($(strip $(MHS35)),1)
DEFINE   += -DPANEL_MHS35
PANEL_OBJ = ILI9486DMADisplay.o
LIBDIR    = $(PORT)/libs/mhs35
else
PANEL_OBJ = ST7789DMADisplay.o
LIBDIR    = $(PORT)/libs/gamepi20
endif

# The sources live one and two directories up; VPATH lets the objects be built
# down here from bare names, flattening the port layer and the shared core into
# this one build directory.
VPATH = $(PORT) $(INFONES)

OBJS  = main.o kernel.o $(PANEL_OBJ) RomMenu.o InfoNES_System_Circle.o

# Emulator core, shared with the pico build, untouched. The 137 mappers are not
# listed: InfoNES_Mapper.cpp #includes them all, so they are not separate
# translation units.
OBJS += InfoNES.o K6502.o InfoNES_pAPU.o InfoNES_Mapper.o

# The core wraps hot functions in the pico-sdk's __not_in_flash_func(), which
# PicoCompat.h defines away - forced into every translation unit rather than
# editing the shared sources.
DEFINE += -include $(PORT)/PicoCompat.h

# Only these two reach InfoNES.cpp itself; the rest of the pico build's
# definitions belong to its own platform layer, which is not built here.
DEFINE += -DNES_FIRST_SCANLINE=0 -DNES_LAST_SCANLINE=239

# An undefined name in #if is silently 0, which once left the scaling compiled
# out while a 320 wide window was still being set. Make that a warning.
DEFINE += -Wundef

# Circle libraries, from the per-target stash rather than the submodule's live
# lib/ tree, so a build does not depend on the submodule's current config.
LIBS  = $(LIBDIR)/libfatfs.a \
	$(LIBDIR)/libusbgadget.a \
	$(LIBDIR)/libusb.a \
	$(LIBDIR)/libinput.a \
	$(LIBDIR)/libsdcard.a \
	$(LIBDIR)/libfs.a \
	$(LIBDIR)/libsound.a \
	$(LIBDIR)/libcircle.a

INCLUDE += -I $(PORT) -I $(PORT)/stdshim -I $(INFONES)

include $(CIRCLEHOME)/app/Rules.mk

-include $(DEPS)
