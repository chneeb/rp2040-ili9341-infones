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

// ---- Panel selection --------------------------------------------------------
//
// Define PANEL_MHS35 (-DPANEL_MHS35, e.g. `make MHS35=1`) to build for the
// goodtft MHS35 (ILI9486, 480x320) on a Pi 2/3. The default is the Waveshare
// GamePi20 (ST7789, 320x240) on a Pi Zero.
//
// CPanelDisplay is whichever driver is selected; both are CDisplay subclasses
// with the same constructor signature, so the kernel treats them alike. The
// ST7789_* names below are the panel config for whichever is selected - the
// historical names are kept to limit churn across the rest of the port.
//
#ifdef PANEL_MHS35
#include "ILI9486DMADisplay.h"
typedef CILI9486DMADisplay CPanelDisplay;
#else
#include "ST7789DMADisplay.h"
typedef CST7789DMADisplay CPanelDisplay;
#endif

// Set to 1 to draw a static test pattern and stop, instead of running the
// emulator. Use it when bringing the panel up: it tells wiring, orientation
// and colour order apart before any emulator code is involved.
#define ST7789_TEST_PATTERN	0

#define NES_WIDTH		256		// what the core renders
#define NES_HEIGHT		240

#ifdef PANEL_MHS35

// ==== goodtft MHS35 / ILI9486, 480x320 landscape ============================
//
// Verified on hardware via the mhs35probe: 16-bit register interface
// (regwidth=16), RGB565, MADCTL 0xE8, landscape. See ILI9486DMADisplay.

#define SKIP_DISPLAY		0

#define ST7789_WIDTH		480
#define ST7789_HEIGHT		320

// Fill the panel: scale the 256x240 NES frame up in both axes (nearest
// neighbour, 256->480 and 240->320). The panel is 3:2 and the NES is 4:3, so
// the picture is a little wide - the same fill-the-panel trade the GamePi20
// makes. Set NES_FILL_WIDTH to 0 for a centred, unscaled 256x240 instead.
#define NES_FILL_WIDTH		1
#define NES_OUT_WIDTH		480
#define NES_OUT_HEIGHT		320

#define DISPLAY_FRAME_SKIP	1

// Wiring (BCM). Backlight is hardwired on - no GPIO. MISO is touch-only.
#define ST7789_DC_PIN		24
#define ST7789_RESET_PIN	25
#define ST7789_BACKLIGHT_PIN	CPanelDisplay::None
#define ST7789_CS_PIN		8
#define ST7789_CPOL		0
#define ST7789_CPHA		0

// 0xE8 = MY | MV | BGR: landscape, matching Linux rotate=270. Confirmed on the
// hardware with the probe's labelled-quadrant test.
#define ST7789_MADCTL		0xE8

// The NES palette is big endian RGB565.
#define ST7789_SWAP_COLOR_BYTES	TRUE

// Simultaneous HDMI output (the Pi 3B has HDMI). The SPI panel stays the
// handheld view; HDMI is a full-60 Hz, correct-4:3 TV output at 640x480, driven
// independently by the VideoCore. First iteration: video only, and graceful -
// if no monitor is present the framebuffer init fails and it is skipped, the
// panel unaffected. Set to 0 to disable. See CLAUDE.md "Simultaneous HDMI".
#define HDMI_OUTPUT		1
#define HDMI_WIDTH		640
#define HDMI_HEIGHT		480

#else	// ==== Waveshare GamePi20 / ST7789, 320x240 ===========================

// Simultaneous HDMI output on the Pi Zero. Experimental here: the Zero is a
// single ARM11 core with no second core to offload to, so the per-frame HDMI
// scale is on top of emulation + the ST7789 scale within one 16.6 ms budget.
// If the game slows or the audio pitch drifts, drop HDMI_WIDTH/HEIGHT. Set
// HDMI_OUTPUT 0 to disable.
#define HDMI_OUTPUT		1
#define HDMI_WIDTH		640
#define HDMI_HEIGHT		480

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
// It costs bandwidth: 153,600 bytes a frame against 122,880, which at 66.7 MHz
// is 18.4 ms against 14.7. The budget is 16.64 ms, so a full width frame does
// not fit in one - InfoNES_LoadFrame() drops a frame when the bus is still
// busy, which halves the picture rate rather than the game speed. See
// ST7789_CLOCK_DIVISOR below.
//
// Set to 0 to go back to a pillarboxed 256 with black bars either side.
#define NES_FILL_WIDTH		1

// Send the picture only every Nth frame. The emulator, its input and its sound
// still run at the full 60 Hz - this changes how smooth the picture looks, not
// how fast the game plays.
//
// What fits, against the 16.64 ms budget of one frame:
//
//   320 px @ 62.5 MHz   19.7 ms   every second frame
//   320 px @ 87.5 MHz   14.0 ms   every frame
//   320 px @ 100 MHz    12.3 ms   every frame, but degrades the panel
//
// The bus rate follows core_freq - see ST7789_CLOCK_DIVISOR below - so this is
// set from config.txt rather than here. core_freq is *not* ignored on this
// board, whatever an earlier version of this comment claimed: it was being put
// after the [pi4]/[cm4] markers, where it only applies to those models.
//
// An earlier round also concluded 100 and 125 MHz were both unstable, but that
// was a missing include leaving the scaling compiled out while a 320 wide
// window was still being set - SetArea then read a 256 wide buffer past its
// end. The clocks were never fairly tested; 100 MHz ran fine afterwards.
#define DISPLAY_FRAME_SKIP	1

#if NES_FILL_WIDTH
#define NES_OUT_WIDTH		320		// what reaches the panel
#else
#define NES_OUT_WIDTH		NES_WIDTH
#endif

// The ST7789 path scales width only; the NES 240 rows map straight to the
// panel's 240.
#define NES_OUT_HEIGHT		NES_HEIGHT

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

// The rate the panel is driven at: a fixed divisor of the core clock as
// measured once at boot, held there afterwards.
//
// Deriving it rather than naming a frequency is what makes the picture rate
// adjustable from the SD card. core_freq in config.txt sets the core, this
// divides it, and the two cannot disagree:
//
//   core_freq=250  ->  62.5 MHz  ->  19.7 ms/frame  ->  30 Hz picture
//   core_freq=350  ->  87.5 MHz  ->  14.0 ms/frame  ->  60 Hz picture
//
// So the picture rate is changed by editing config.txt - reachable over USB
// transfer mode, no rebuild and no reflash. See boot/config.txt, and mind that
// core_freq must sit above the [pi4]/[cm4] markers or the firmware ignores it.
//
// The rate still has to be actively *held*. Circle fixes the SPI divisor at
// init and never revisits it, so what comes out is the current core clock over
// that stale divisor - and an unpinned core moves on its own, roughly 250 MHz
// idle and 400 under load. CKernel::WaitForNextFrame() re-aims the bus once a
// second against the target worked out at boot, so it stays put regardless.
#define ST7789_CLOCK_DIVISOR	4

// Never ask for more than this, whatever the core turns out to be doing. 100
// MHz visibly degrades this panel, and an unpinned core caught boosting at
// boot would divide to exactly that. 87.5 MHz passes, 100 does not.
#define ST7789_CLOCK_CEILING	90000000

// Used only if the core clock cannot be read at all, which would otherwise
// leave the bus at nothing. The Zero's default.
#define ST7789_CLOCK_CORE_ASSUMED	250000000

// The GamePi20 has the panel mounted upside down. MY | MV | ML turns the
// picture around at no cost; 0x70 is the same layout the right way up.
#define ST7789_MADCTL		0xB0

// TRUE selects the RGB565_BE colour model. The NES palette carried over from
// the pico build is big endian RGB565; circle-arcade needs FALSE instead,
// because its LMI assets are plain RGB565.
#define ST7789_SWAP_COLOR_BYTES	TRUE

#endif	// PANEL_MHS35 / GamePi20

// ---- Common ----------------------------------------------------------------

// The scaled frame fills the panel, so the offsets are 0 on both; the general
// form leaves room for a smaller centred image (NES_FILL_WIDTH 0).
#define NES_OFFSET_X		((ST7789_WIDTH - NES_OUT_WIDTH) / 2)
#define NES_OFFSET_Y		((ST7789_HEIGHT - NES_OUT_HEIGHT) / 2)
