//
//  DisplayConfig.h
//
//  The Waveshare GamePi20's ST7789 panel, driven over SPI with DMA.
//
//  Pin numbers are SoC (BCM) numbers, not header positions. These values were
//  verified on the hardware in the circle-arcade port; see that project's
//  CLAUDE.md for how each of them was arrived at.
//
#pragma once

#include "ST7789DMADisplay.h"

// Set to 1 to draw a static test pattern and stop, instead of running the
// emulator. Use it when bringing the panel up: it tells wiring, orientation
// and colour order apart before any emulator code is involved.
#define ST7789_TEST_PATTERN	0

// The panel is a 240x320 ST7789VW driven in landscape. MADCTL's MV bit swaps X
// and Y, so it presents as 320x240, and being a true 240x320 part it needs no
// GRAM offset - unlike the common 240x240 variant, which wants 80 rows.
#define ST7789_WIDTH		320
#define ST7789_HEIGHT		240

// The NES renders 256x240. Pillarboxing that rather than stretching it to 320
// is both simpler and faster: 256 wide is 122,880 bytes a frame against
// 153,600, which at 62.5 MHz is 15.7 ms against 19.7 - the difference between
// clearing 60 fps and not being able to.
#define NES_WIDTH		256
#define NES_HEIGHT		240
#define NES_OFFSET_X		((ST7789_WIDTH - NES_WIDTH) / 2)
#define NES_OFFSET_Y		((ST7789_HEIGHT - NES_HEIGHT) / 2)

// GamePi20 wiring. MISO (BCM 9) is not connected, which is fine: the panel is
// only ever written to.
#define ST7789_DC_PIN		25
#define ST7789_RESET_PIN	27
#define ST7789_BACKLIGHT_PIN	24

// Chip select is driven by hand as an ordinary output, so it can be held low
// across the several transfers one frame is split into. The SPI peripheral
// takes at most 0xFFFF bytes at a time, and letting it toggle CE0 per transfer
// would break the RAMWR stream at every chunk boundary.
#define ST7789_CS_PIN		8	// CE0 = BCM 8

// CPOL 0 is right because chip select is wired.
#define ST7789_CPOL		0
#define ST7789_CPHA		0

// Circle derives the SPI divisor from the measured core clock, so on a 250 MHz
// core the reachable rates are 62.5, 41.7 and 31.25 MHz. 62.5 MHz is well above
// what the ST7789VW is specified for; if the picture tears, flickers or shows
// intermittent noise, step down to 41666666. A marginal clock looks like a
// wiring fault, not like a clock problem.
#define ST7789_CLOCK_SPEED	62500000

// The GamePi20 has the panel mounted upside down. MY | MV | ML turns the
// picture around at no cost; 0x70 is the same layout the right way up.
#define ST7789_MADCTL		0xB0

// TRUE selects the RGB565_BE colour model. The NES palette carried over from
// the pico build is big endian RGB565; circle-arcade needs FALSE instead,
// because its LMI assets are plain RGB565.
#define ST7789_SWAP_COLOR_BYTES	TRUE
