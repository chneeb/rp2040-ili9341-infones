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

// Diagnostic: run the emulator and its sound, but never send a frame to the
// panel. If a background noise in the audio disappears with this set, the
// noise is the display's SPI bursts coupling into the audio path rather than
// anything in the sample data - the picture simply freezes on whatever was
// last drawn.
#define SKIP_DISPLAY		0

// The panel is a 240x320 ST7789VW driven in landscape. MADCTL's MV bit swaps X
// and Y, so it presents as 320x240, and being a true 240x320 part it needs no
// GRAM offset - unlike the common 240x240 variant, which wants 80 rows.
#define ST7789_WIDTH		320
#define ST7789_HEIGHT		240

// The NES renders 256x240, and its pixels are not square: on a 4:3 set each is
// displayed a quarter wider than it is tall, so 320x240 is what a CRT actually
// showed. Filling the panel is the more faithful picture, not a distortion -
// and 256 to 320 is exactly 4:5, so every fourth pixel is doubled and nothing
// else shifts.
//
// It costs bandwidth: 153,600 bytes a frame against 122,880, which is 19.7 ms
// against 15.7 at 62.5 MHz. The budget is 16.64 ms, so filling the width needs
// a faster SPI clock - see ST7789_CLOCK_SPEED below, and note that it needs a
// matching core_freq in config.txt.
//
// Set to 0 to go back to a pillarboxed 256 with black bars either side.
#define NES_FILL_WIDTH		1

// Send the picture only every Nth frame. The emulator, its input and its sound
// still run at the full 60 Hz - this changes how smooth the picture looks, not
// how fast the game plays.
//
// What fits, against the 16.64 ms budget of one frame:
//
//   256 px @ 62.5 MHz   15.7 ms   every frame
//   320 px @ 66.7 MHz   18.4 ms   every second frame
//   320 px @ 100 MHz    12.3 ms   every frame
//
// 100 MHz needs the core pinned at 400 (force_turbo=0 in config.txt; core_freq
// is ignored on this board). An earlier round concluded 100 and 125 MHz were
// both unstable, but that was a missing include leaving the scaling compiled
// out while a 320 wide window was still being set - SetArea then read a 256
// wide buffer past its end. The clocks were never fairly tested.
#define DISPLAY_FRAME_SKIP	1

#define NES_WIDTH		256		// what the core renders
#define NES_HEIGHT		240

#if NES_FILL_WIDTH
#define NES_OUT_WIDTH		320		// what reaches the panel
#else
#define NES_OUT_WIDTH		NES_WIDTH
#endif

#define NES_OFFSET_X		((ST7789_WIDTH - NES_OUT_WIDTH) / 2)
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

// Circle divides the *measured* core clock by this and truncates, so the rate
// that comes out depends on core_freq in config.txt:
//
//   core_freq=250  ->  125, 62.5, 41.7 MHz
//   core_freq=400  ->  200, 100, 66.7, 50 MHz
//
// A full width frame is 153,600 bytes and needs at least 74 MHz to fit in the
// 16.64 ms budget, so NES_FILL_WIDTH wants core_freq=400 and 100 MHz.
//
// core_freq=350 was tried and the firmware ignored it, with and without
// force_turbo. 400 is the native turbo rate, so it stands a better chance.
//
// **Pin both core_freq and core_freq_min.** The core clock is dynamic - around
// 250 idle and 400 under load - and Circle measures it once at init, computes
// the divisor and never looks again. An unpinned core that boosts afterwards
// silently multiplies the bus rate, which is the likeliest reason 125 MHz once
// looked intermittent rather than simply too fast.
//
// WARNING: asking for 87.5 MHz without setting core_freq does not give 62.5 -
// it truncates 250/87.5 to a divisor of 2 and runs the bus at 125 MHz. The
// config.txt line is not optional.
//
// All of this is far beyond what the ST7789VW is specified for. If the picture
// tears, flickers or shows intermittent noise, drop back a step - a marginal
// clock looks like a wiring fault rather than a clock problem.
#define ST7789_CLOCK_SPEED	100000000	// core 400 pinned, gives 400/4

// The GamePi20 has the panel mounted upside down. MY | MV | ML turns the
// picture around at no cost; 0x70 is the same layout the right way up.
#define ST7789_MADCTL		0xB0

// TRUE selects the RGB565_BE colour model. The NES palette carried over from
// the pico build is big endian RGB565; circle-arcade needs FALSE instead,
// because its LMI assets are plain RGB565.
#define ST7789_SWAP_COLOR_BYTES	TRUE
