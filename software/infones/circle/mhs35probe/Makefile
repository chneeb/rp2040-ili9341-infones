#
# Makefile - MHS35 / ILI9486 colour-format probe
#
# Standalone Circle app, shares nothing with the InfoNES port. Build Circle for
# the Pi you are testing on FIRST (see README.md), then run make here.
#

CIRCLEHOME = ../../../../circle

# The real port driver is compiled in from one directory up, so this stage
# tests the actual CILI9486DMADisplay class, not a probe-only copy.
OBJS = main.o kernel.o ../ILI9486DMADisplay.o

LIBS = $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
