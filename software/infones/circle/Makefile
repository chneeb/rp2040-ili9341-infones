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

OBJS	= main.o kernel.o ST7789DMADisplay.o

# The platform layer proper: the 13 functions of InfoNES_System.h.
# Uncomment as they are written.
#OBJS	+= InfoNES_System_Circle.o

# Emulator core, shared with the pico build.
#OBJS	+= $(INFONES)/InfoNES.o $(INFONES)/K6502.o $(INFONES)/InfoNES_pAPU.o
#OBJS	+= $(INFONES)/InfoNES_Mapper.o
#OBJS	+= $(patsubst %.cpp,%.o,$(wildcard $(INFONES)/mapper/*.cpp))

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	  $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/libcircle.a

INCLUDE	+= -I $(INFONES)

include $(CIRCLEHOME)/app/Rules.mk

-include $(DEPS)

clean_all:
	@rm -f *.o *.d $(INFONES)/*.o $(INFONES)/*.d \
	       $(INFONES)/mapper/*.o $(INFONES)/mapper/*.d
