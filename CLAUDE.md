# rp2040-ili9341-infones — Claude Notes

## Project Overview
RP2350-based handheld NES emulator using InfoNES, driving an ST7789 (or ILI9341) LCD via overclocked SPI. Despite the repo name referencing RP2040/ILI9341, the actual hardware is an RP2350 with an ST7789 display.

**There are two ports in this repository.** Everything below describes the
pico-sdk one unless it says otherwise. The second, on branch `circle-gamepi20`,
runs the same emulator bare metal on a Raspberry Pi Zero via Circle — see
[The Circle / GamePi20 port](#the-circle--gamepi20-port). They share the
emulator core and the 137 mappers untouched; only the platform layer differs.

## Key Files
- `software/infones/main.cpp` — main application: display init, DMA rendering loop, InfoNES callbacks, controller input, SD card init, ROM loading
- `software/infones/CMakeLists.txt` — build config; selects LCD controller, defines all pin assignments
- `software/infones/menu.cpp` / `menu.h` / `rom_selector.h` — SD card file browser menu; `menu()` never returns, reboots via watchdog on game selection; writes chosen path to `/currentloadedrom.txt`
- `software/infones/RomLister.cpp` / `RomLister.h` — enumerates `.nes` files on SD card for menu display
- `software/infones/FrensHelpers.cpp` — shared helper utilities
- `software/infones/drivers/sdcard/sdcard.c` — SD card driver (shared SPI bus handling)

## Hardware Configuration (Current)

### MCU & Clocks
- **MCU**: RP2350 (Cortex-M33) — default; RP2040 also supported (see Build)
- **CPU clock**: derived from chip via `PICO_RP2350` cmake variable — 300 MHz for RP2350, 252 MHz for RP2040
- **ST7789 SPI clock**: 80 MHz (`DISPLAY_SPI_CLOCK_SPEED_HZ 80000000`); on RP2040 actual rate is ~63 MHz (252 MHz sys / 4)
- **ILI9341 SPI clock**: 63 MHz (`DISPLAY_SPI_CLOCK_SPEED_HZ 63000000`)

### Pins: ST7789 on spi1 (shared with SD card)
| Signal       | GPIO |
|--------------|------|
| LCD SPI      | spi1 |
| LCD DC       | 8    |
| LCD CS       | 9    |
| LCD CLK      | 10   |
| LCD MOSI     | 11   |
| LCD RST      | 15   |
| LCD BL       | 13   |
| LCD MISO     | -1   |
| SD CS        | 22   |
| SD SCK       | 10   |
| SD MOSI      | 11   |
| SD MISO      | 12   |
| Touch CS     | 16   |
| Controller SDA | 26 |
| Controller SCL | 27 |

**LCD MISO is -1**: `LCD_MISO = -1` prevents the firmware from configuring GPIO 12 as SPI for the display; the SD card still uses it as MISO.

**Touch CS (GP16) must be driven HIGH at startup**: The XPT2046 touch controller shares spi1 with the LCD and SD card. If GP16 floats low, the XPT2046 responds to all SPI traffic and drives MISO low, causing `wait_ready()` in `disk_initialize()` to time out and SD card init to fail with `FR_NOT_READY`. `main()` drives GP16 HIGH before `display_init()`.

### Input: NES Mini Classic clone (i2c1, addr 0x52)
Init sequence (order matters):
```
{0xF0, 0x55}
{0xFB, 0x00}
{0xFE, 0x03}  ← required for this clone
```
Read: write `0x00`, wait 200 µs, read **8 bytes** (not 6).

Button mapping — active low, bytes 6 and 7:
| Button | Byte | Mask |
|--------|------|------|
| Up     | 7    | 0x01 |
| Down   | 6    | 0x40 |
| Left   | 7    | 0x02 |
| Right  | 6    | 0x80 |
| B      | 7    | 0x40 |
| A      | 7    | 0x10 |
| Select | 6    | 0x10 |
| Start  | 6    | 0x04 |

## Architecture

### Display Rendering
- NES native output: 256 pixels wide × 240 pixels tall
- **320-wide targets** (ORIGINAL_RP2040, PICO_RESTOUCH): scale 256→320 nearest-neighbor in-place (right-to-left); 4px border top/bottom; window columns 0–319, rows 4–235 (232 rows)
- **240-wide target** (WAVESHARE_LCD13): crop 256→240 (drop 8px overscan each side: `fb[i+8]`); full height; window columns 0–239, rows 0–239 (240 rows)
- ST7789 uses `DISPLAY_INVERT` define (set in CMakeLists.txt for ST7789 targets)

### Scanline DMA Flow
- Two ping-pong buffers: `scanline_buf_internal_1` / `scanline_buf_internal_2` (WORD[**`SCANLINE_BUF_WORDS`**] — `DISPLAY_WIDTH` for 320-wide targets, 256 for 240-wide targets; must be at least `DISPLAY_WIDTH` because the 320-wide scaling loop writes `fb[0..319]` in-place)
- InfoNES renders `NES_FIRST_SCANLINE`–`NES_LAST_SCANLINE` only, controlled in `InfoNES.cpp`
- `InfoNES_PreDrawLine(line)` — sets InfoNES line buffer (buf1 for even, buf2 for odd lines)
- `InfoNES_PostDrawLine(line)` — scales or crops in-place, waits for previous DMA, then starts DMA
- DMA: `DMA_SIZE_8`, `DISPLAY_WIDTH * 2` bytes per scanline, DREQ-paced to SPI TX FIFO
- At 80 MHz SPI, 320-wide: ~14.8 ms/frame → ceiling ~67 fps; 240-wide: ~11.5 ms/frame → ceiling ~87 fps (both sufficient for NES 60 fps)

### Frame Rate
- `speed_control()` in `InfoNES_LoadFrame()` caps at 60 fps (waits if frame finishes early)
- No display frame skipping is currently active (all frames DMA'd to display)
- The 60 fps cap + 80 MHz SPI + 300 MHz RP2350 achieves correct NES game speed; RP2040 at 252 MHz is sufficient but closer to the margin

### ROM Loading Flow
1. At startup, `initSDCard()` is called if `SDCARD_PIN_SPI0_CS >= 0` (i.e., on targets with an SD slot). `isFatalError` is set to `!initSDCard()`.
2. On WAVESHARE_LCD13 (no SD slot), `isFatalError = true` always — the flash ROM at `NES_FILE_ADDR` is used directly.
3. If `watchdog_caused_reboot() && !isFatalError`: read `/currentloadedrom.txt` from SD card for the ROM path and start the emulator immediately.
4. Otherwise: show the file-browser menu (`menu()`). If `isFatalError` (no SD card), the menu falls through to run the flash ROM.
5. `menu()` never returns — when a game is selected it writes the path to `/currentloadedrom.txt` and calls `watchdog_reboot()`.
6. After `InfoNES_Main()` exits (player quit), `selectedRom` is cleared and the menu is shown again.

### Shared SPI Bus (CRITICAL)
The SD card and LCD share **the same SPI bus (spi1)** with CLK/MOSI/MISO on GPIO 10/11/12. They use different CS pins (LCD CS=9, SD CS=22). If display CS is LOW while the SD card is accessed, the display receives SD card SPI traffic as pixel data, corrupting the write pointer.

**SD CS early init**: GPIO 22 is driven HIGH in `main()` before `display_init()` to prevent the SD card (which retains SPI mode across watchdog reboots) from receiving display init traffic on the shared bus.

**Menu SD access**: `menu.cpp` calls `display_deselect_for_sd()` (drives LCD CS HIGH, waits for SPI idle) before every SD card access on `SHARED_SPI_BUS` targets. DMA scanline rendering leaves LCD CS LOW after each frame, so this guard is required to avoid corrupting the display write pointer.

### SPI Baudrate After SD Card Access
The SD card driver switches to `CLK_SLOW` (100 kHz) for init then `CLK_FAST` (30 MHz) for data. After `initSDCard()` returns (all code paths), the SPI baudrate is restored to `DISPLAY_SPI_CLOCK_SPEED_HZ`.

## Fixed Bugs

### ST7789 Horizontal Image Wrap (CONFIRMED FIX)
**Symptom:** Image content shifted 20–40 pixels right with wrap-around.

**Root cause:** Frame timing init functions left display CS LOW. SD card traffic on the shared bus advanced the display write pointer by an unpredictable amount.

**Fix:**
1. Both `st7789_infones_frame_timing_register_init()` and `ili9341_infones_frame_timing_register_init()` no longer leave CS LOW.
2. `InfoNES_PostDrawLine` ST7789 path: on `line == 4`, drains SPI TX FIFO then calls `display_set_address` to reset the write pointer, then sets DC=1, CS=0 before DMA.

### SD Card Init Failure — Shared SPI Bus (CONFIRMED FIX)
**Symptom:** `f_mount()` returned `FR_NOT_READY` (3).

**Root cause 1:** `sdcard.c` called `gpio_init()` + `gpio_set_function()` on shared SCK/MOSI/MISO, resetting their SPI function.

**Root cause 2:** Just `spi_set_baudrate()` without a peripheral reset left residual SPI state from display operations, corrupting SD card CMD0 responses.

**Root cause 3:** GPIO 22 (SD CS) floated during `display_init()`. SD cards in SPI mode (retained across watchdog reboots) held MISO low while processing received display traffic, causing `wait_ready()` to time out.

**Root cause 4:** XPT2046 touch controller CS (GP16) was never driven HIGH. The touch controller shares spi1; with CS floating low it drove MISO low continuously, blocking all SD card init attempts.

**Fix:** `init_spi()` in `sdcard.c` calls `spi_init(SDCARD_SPI_BUS, CLK_SLOW)` (resets peripheral) but does NOT call `gpio_init()`/`gpio_set_function()` on the shared SCK/MOSI pins. It does call `gpio_set_function(SDCARD_PIN_SPI0_MISO, GPIO_FUNC_SPI)` + `gpio_pull_up()` for MISO (GPIO 12), which is safe because `display_init()` skips MISO when `LCD_MISO == -1`. After `spi_init()`, `disk_initialize()` sleeps 100 ms to let the card stabilize, then sends 160 dummy clocks (20 bytes, CS=HIGH) instead of the spec minimum 74 to flush any mid-response state from a watchdog reboot. `main()` drives SD CS (GP22) and touch CS (GP16) HIGH before `display_init()`.

### Scanline Buffer Overflow on 320-Wide Targets (CONFIRMED FIX)
**Symptom:** Every odd scanline had its left ~80 columns showing content from the right edge of the preceding even scanline — visible as repeated/garbled image data on the right side of the screen.

**Root cause:** Buffers were sized `[256]` (NES native width), but the 320-wide in-place scaling loop writes `fb[0..319]`. The overflow for even lines (buf1) spilled into `buf2[0..63]`, which InfoNES had just filled with the next odd line's pixel data. The odd line's scaling then read those corrupted bytes as source pixels for display columns 0–79, producing the wrong content.

**Fix:** Buffers are now sized via `SCANLINE_BUF_WORDS` = `max(DISPLAY_WIDTH, 256)`, resolving to 320 for 320-wide targets and 256 for 240-wide targets (where the crop path never writes past index 239).

## Build
```bash
mkdir software/infones/build && cd software/infones/build
cmake .. -DPICO_BOARD=pico2              # RP2350 (default)
# cmake .. -DPICO_BOARD=pico            # RP2040
make
# Flash infoNES.uf2 to the board
```

**ROM loading (PICO_RESTOUCH / ORIGINAL_RP2040):** Copy `.nes` files to an SD card (FAT32). On first boot the file-browser menu appears; select a game to play. The selection is saved to `/currentloadedrom.txt` and the device reboots into the emulator. The menu reappears after the game exits.

**ROM loading (WAVESHARE_LCD13 / no SD card):** Flash a single ROM directly to flash with picotool:
```bash
picotool load path/to/game.nes -t bin -o 0x10080000
```

`CPU_FREQ_KHZ` is set automatically from the cmake `PICO_RP2350` variable (300 MHz for RP2350, 252 MHz for RP2040) — it is not part of `HARDWARE_TARGET`.

## Hardware Target Selection (CMakeLists.txt)

Set `HARDWARE_TARGET` via `-DHARDWARE_TARGET=...` (or edit the default in CMakeLists.txt). **Delete the build folder before switching targets or boards.**

`HARDWARE_TARGET` describes peripheral wiring only (pins, display type, controller). Board/chip is selected separately via `-DPICO_BOARD`.

### Configuration comparison

| Aspect | ORIGINAL_RP2040 | PICO_RESTOUCH | WAVESHARE_LCD13 |
|---|---|---|---|
| Tested MCU | RP2040 | RP2350 | RP2350 |
| LCD | ILI9341 320×240 | ST7789 320×240 | ST7789 240×240 |
| LCD SPI / DC/CS/CLK/MOSI | spi0 / 20/17/18/19 | spi1 / 8/9/10/11 | spi1 / 8/9/10/11 |
| LCD RST / BL | 21 / 22 | 15 / 13 | 12 / 13 |
| SD SPI bus | spi1 (separate) | spi1 (shared) | none |
| SD CS | 13 | 22 | — |
| Touch CS | none | GP16 | none |
| Controller | none | NES Mini (i2c1, GP26/27) | GPIO buttons+joystick |
| CPU clock | from chip (252/300 MHz) | from chip (252/300 MHz) | from chip (252/300 MHz) |
| VREG | no | VREG_VOLTAGE_1_20 | VREG_VOLTAGE_1_20 |
| `SHARED_SPI_BUS` | — | ✓ | — |
| NES scanlines rendered | 4–235 (232 rows) | 4–235 (232 rows) | 0–239 (240 rows) |

### Waveshare Pico LCD 1.3" button mapping

| NES button | Physical input |
|---|---|
| Up/Down/Left/Right | Joystick (GP2/18/16/20) |
| A | Button A (GP15) |
| B | Button B (GP17) |
| Select | Button X (GP19) |
| Start | Button Y (GP21) |

Joystick center (GP3) is currently unused; extend `key_init()` / `InfoNES_PadState()` if needed.

### Display orientation / DISPLAY_ADDRESS_MODE

Each target has its own `DISPLAY_ADDRESS_MODE` defined in `main.cpp`:

| Target | DISPLAY_ADDRESS_MODE | Value |
|---|---|---|
| ORIGINAL_RP2040 (ILI9341) | `DCS_ADDRESS_MODE_BGR \| DCS_ADDRESS_MODE_SWAP_XY` | 0x28 |
| PICO_RESTOUCH (ST7789 320×240) | `DCS_ADDRESS_MODE_RGB \| DCS_ADDRESS_MODE_SWAP_XY \| DCS_ADDRESS_MODE_MIRROR_Y` | 0xA0 |
| WAVESHARE_LCD13 (ST7789 240×240) | `DCS_ADDRESS_MODE_MIRROR_X \| DCS_ADDRESS_MODE_SWAP_XY` | 0x60 |

**WAVESHARE_LCD13 GRAM offset**: No offset needed (`DISPLAY_OFFSET_X=0`, `DISPLAY_OFFSET_Y=0`). The MX mirror in `0x60` compensates for the 80-row portrait-mode GRAM offset of the ST7789 240×240 panel, so `CASET(0, 239)` and `RASET(0, 239)` map directly to the full visible area.

**PICO_RESTOUCH / ORIGINAL_RP2040**: also use `DISPLAY_OFFSET_X=0`, `DISPLAY_OFFSET_Y=0` — their panels expose the full address range without any offset.

### Key compile-time defines (set by CMakeLists.txt per target)

| Define | Purpose |
|---|---|
| `HARDWARE_TARGET_<name>` | e.g. `HARDWARE_TARGET_PICO_RESTOUCH` |
| `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` | Display pixel dimensions |
| `NES_FIRST_SCANLINE` / `NES_LAST_SCANLINE` | NES scanline render window |
| `CPU_FREQ_KHZ` | System clock in kHz — set from `PICO_RP2350` cmake variable, not from `HARDWARE_TARGET` |
| `SHARED_SPI_BUS` | LCD and SD share a bus — enables shared-bus workarounds |
| `OVERCLOCK_VREG` | Enable `vreg_set_voltage(VREG_VOLTAGE_1_20)` before clock boost |
| `CONTROLLER_NUNCHUCK` | I2C NES Mini Classic input |
| `CONTROLLER_GPIO_BUTTONS` | GPIO button+joystick input |
| `NUNCHUCK_I2C_BUS` / `NUNCHUCK_SDA` / `NUNCHUCK_SCL` | Nunchuck I2C bus and pins |
| `BTN_A/B/X/Y` / `JOY_UP/DOWN/LEFT/RIGHT/CTR` | Waveshare button/joystick GPIO pins |

---

## The Circle / GamePi20 port

Branch `circle-gamepi20`. The same InfoNES core, running bare metal on a
Raspberry Pi Zero (W) in a Waveshare GamePi20, through
[Circle](https://github.com/rsta2/circle) instead of the pico-sdk. No OS.

Working on hardware: picture, colours and buttons. Not done: sound, frame
pacing, ROM selection.

The display driver, pin numbers and the bring-up approach come from the
`circle-arcade` project (github.com/chneeb/circle-arcade), which was taken
through the same hardware first. Its CLAUDE.md documents how each value was
arrived at and is worth reading alongside this.

### Layout

```
circle/                                  submodule, pinned to Step51
configure-gamepi20.sh                    configures and builds Circle
software/infones/
├── InfoNES.cpp, K6502.cpp, mapper/      shared with the pico build, untouched
├── InfoNES_System.h                     the porting seam: 13 functions
├── main.cpp, CMakeLists.txt             pico platform layer
├── linux/InfoNES_System_Linux.cpp       reference port, not built
└── circle/                              this port
    ├── Makefile
    ├── main.cpp, kernel.{cpp,h}
    ├── InfoNES_System_Circle.cpp        the 13 functions, plus NesPalette
    ├── ST7789DMADisplay.{h,cpp}         panel driver, frames over DMA
    ├── DisplayConfig.h, InputConfig.h   pins and panel settings
    ├── GamePi20.h                       bridge: emulator <-> kernel
    ├── pico.h, PicoCompat.h             stand-ins, see below
    └── stdshim/                         four C++ headers, see below
```

CMake never looks at `circle/`, so the two builds coexist. The Circle Makefile
reaches the shared sources by relative path, so nothing is duplicated and
`git fetch upstream` still merges.

### Build

```bash
./configure-gamepi20.sh          # from the repository root, once
cd software/infones/circle && make
```

Circle's Rules.mk compiles in place, so this leaves `.o`/`.d` files next to the
shared sources. They are gitignored.

SD card: `bootcode.bin`, `start.elf`, `fixup.dat` (from `make` in
`circle/boot`), `config.txt` (a copy of `circle/boot/config32.txt`),
`cmdline.txt`, `kernel.img`, and the games in a `nes/` directory.

`config.txt` also pins the core clock (`core_freq=250`, `core_freq_min=250`,
above the `[model]` markers) so the SPI rate derived from it stays put. See the
display notes.

**Hold Up while powering on** to boot into USB mass storage mode instead of the
emulator - see [USB transfer mode](#usb-transfer-mode).

### Four things the core needs that it does not advertise

Each of these fails in a way that does not point at its cause:

- **`<pico.h>` is included directly** by `InfoNES.cpp`, `K6502.cpp` and
  `InfoNES_Mapper.cpp`, for `__not_in_flash_func()`. The local `pico.h` takes
  that name on the include path and defines the pico macros away. It also has
  to supply `<stdint.h>`, which the real one pulls in — without it `uint16_t`
  is undeclared and the errors point at `makeTag` instead.
- **The 137 mappers are `#include`d into `InfoNES_Mapper.cpp`**, so they are
  not separate translation units. Listing them as objects gives a wall of
  `'BYTE' does not name a type`.
- **`<cstddef>`, `<cstdio>`, `<tuple>` and `<algorithm>`** are included, but
  only `std::min` and `std::max` are ever used. Circle builds with
  `-nostdinc++`; raising `STDLIB_SUPPORT` to 3 works but links all of libstdc++
  and then wants `abort`, `getenv`, `printf` and RTTI. `stdshim/` supplies the
  four headers instead, and Circle stays at its default level.
- **`NesPalette` belongs to the platform layer**, not the core. It is in
  `InfoNES_System_Circle.cpp`.

### The palette is big endian RGB565

Carried over from the pico build, and **that is why this port sets
`ST7789_SWAP_COLOR_BYTES` to TRUE where circle-arcade sets it FALSE** — that
project's LMI assets are plain RGB565.

Read as plain RGB565 the palette is wrong in a specific way: red and blue
exchange, so white stays white while sky blue turns beige. Checked against the
real 2C02 colours, entry 0x21 comes out (255, 230, 238) read straight and
(57, 190, 255) read byte swapped, against a true (63, 191, 255).

The pico build masks each entry with 32767. That is dropped here: measured
against the real palette it roughly doubles the error.

### Video path

The core is scanline based. `InfoNES_PreDrawLine(line)` points its line buffer
at row `line` of a full 256x240 frame, so the frame accumulates with no copying,
and `InfoNES_LoadFrame()` hands the whole thing over at once.

That is deliberately simpler than the pico build's ping-ponged scanline DMA,
which exists because an RP2350 has 264 KB of RAM. A Pi has 512 MB, and
`ST7789DMADisplay::SetArea` already returns while the frame is still going out,
so the next frame is emulated during the transfer either way.

#### Full width, and the frame rate it costs

The picture fills the panel: `NES_FILL_WIDTH` is 1. That is the more faithful
image rather than a stretch - NES pixels are not square, and on a 4:3 set
256x240 is displayed as 320x240 - and 256 to 320 is exactly 4:5, so every fourth
pixel doubles and nothing else shifts. The scale uses a precomputed
source-column table, the same approach as nesemu (`spi_lcd.c`, `scaleX[]`).

It costs bandwidth. Against the 16.64 ms budget of one frame:

| | frame time | |
|---|---|---|
| 320 px @ 62.5 MHz (default) | 19.7 ms | every second frame - 30 Hz picture |
| 320 px @ 87.5 MHz | 14.0 ms | every frame - 60 Hz picture |
| 320 px @ 100 MHz | 12.3 ms | every frame, but degrades the panel |

At the default the picture runs at 30 Hz while the game, input and audio stay at
60. 60 Hz picture needs about 74 MHz (naively; the per-frame scale and copy eat
into the window, so more like 80+), which means pinning the core higher -
`core_freq=350` and `ST7789_TARGET_CLOCK` of 87500000 gives divisor 4 - at a
cost in battery. 100 MHz has the bandwidth but visibly degrades this panel.

**The display rate looks after itself.** `InfoNES_LoadFrame()` drops a frame if
the previous one is still going out, rather than waiting for the bus. Waiting
would stall the emulator and cost the game its speed and its audio pitch;
dropping only costs smoothness. So the picture runs as fast as the bus allows
and the game always runs at the right speed.

That matters because **the core clock does not stay put on its own**. It is
roughly 250 MHz idle and 400 under load, and Circle measures it once at init,
computes the SPI divisor and never looks again - so an uncontrolled core takes
the bus rate with it. At divisor 4 that is 62.5 MHz at core 250 and 100 MHz at
core 400, and 100 degrades this panel until the core drops back.

Two things keep it in hand, and either would do on its own:

- **`config.txt` pins the core** at 250 with `core_freq=250` and
  `core_freq_min=250`. **These must sit above the `[pi4]`/`[cm4]` markers.**
  Anything after a `[model]` filter applies only to that model, so a `core_freq`
  at the end of the file is silently ignored on a Zero - which is exactly what
  happened for several rounds here, and is why an earlier version of this note
  wrongly claimed the firmware ignores `core_freq` altogether. It does not; it
  was in the wrong section.
- **`CST7789DMADisplay::SetTargetClock()` re-aims the bus** once a second from
  `CKernel::WaitForNextFrame()`, holding it at `ST7789_TARGET_CLOCK` whatever
  the core is doing. Re-requesting the rate is not enough by itself:
  `CSPIMasterDMA` divides by the core rate it captured at construction, equally
  stale, so the request is scaled by how far the core has moved since. With the
  core pinned this is a no-op; it is the belt to config.txt's braces, and it
  covers the case where the pin is ever lost.

The menu's status line shows the current core clock (which still moves) and the
held bus rate.

##### The bug that made all of this look like a clock problem

`InfoNES_System_Circle.cpp` did not include `DisplayConfig.h`, so
`NES_FILL_WIDTH` was undefined there - and an undefined name in `#if` is
silently 0. The scaling was compiled out and the 256 wide buffer was sent, while
`kernel.cpp`, which does include the config, set a 320 wide window. `SetArea`
then read 153,600 bytes out of a 122,880 byte buffer, past its end and with
every row offset by 64 pixels.

That garbling was blamed on the SPI clock, and 87.5, 100 and 125 MHz were each
condemned on the strength of it. Once the include was added, 100 MHz ran fine.
**`-Wundef` would have caught this** and is worth adding to the build.

`NES_FIRST_SCANLINE=0` and `NES_LAST_SCANLINE=239` are set in the Makefile;
they are the only two of the pico build's compile definitions that reach
`InfoNES.cpp` itself.

### Display

ST7789VW, 240x320 native, driven as 320x240 landscape. `ST7789DMADisplay` sends
frames over DMA rather than the polled writes Circle's own `CST7789Display`
uses. Three things about Circle's DMA SPI that the headers do not make obvious:

- `StartWriteRead` asserts `nCount <= 0xFFFF`, but a frame is bigger, so it
  goes out in chunks of 61,440 chained through the completion routine.
- It also asserts a non-null read buffer. Not waste: with no RX DMA the receive
  FIFO is never drained and the controller stalls.
- Chip select is driven by hand so it can stay low across every chunk of one
  frame; letting the peripheral toggle CE0 would break the RAMWR stream at each
  chunk boundary.

The panel is mounted upside down; MADCTL `0xB0` turns the picture around in
hardware at no cost. `ST7789_TEST_PATTERN` in `DisplayConfig.h` draws bars,
a border and corner markers and stops — the quickest way to separate wiring,
orientation and colour order from emulator problems.

### Input

The board's buttons on GPIO, sampled once per frame in `CKernel::ReadPad()` and
handed over by `InfoNES_PadState()`. Active low with internal pull-ups; no
debounce needed at that rate. The map is the table at the top of `kernel.cpp`
and is the only place any of this lives.

| Board button | BCM | NES |
|---|---|---|
| A | 23 | **B** |
| B | 4 | **A** |
| X | 22 | B |
| Y | 17 | A |
| Up / Down / Left / Right | 12 / 20 / 21 / 13 | D-pad |
| SELECT / START | 16 / 26 | Select / Start; together, quit to the ROM menu |
| TL / TR | 5 / 6 | volume, not the pad — see below |

**A and B are crossed on purpose.** The board's silkscreen does not follow the
convention NES games expect, where A is the primary action and falls under the
thumb on the right; mapped literally it plays wrong. Swapping the two lines
back gives the literal mapping.

X and Y are not spare in any useful sense — they duplicate A and B. If
something better comes up (a reset combo, an on-screen indicator) they are the
buttons to take.

**TL and TR work the volume**, not the pad: TL down, TR up, both together to
mute and unmute. Nothing is lost by taking them, as SELECT and START both have
buttons of their own.

A step is taken when a button is **released**, not pressed. Two buttons are
never pressed at quite the same moment, so acting on the press would step the
volume for whichever arrived first every single time the mute chord was used.
Waiting for the release lets the chord be recognised first and the step
suppressed. The cost is that the change lands on the release, which nobody can
perceive for a volume control and would be wrong for anything twitchy.

### Sound

PWM on GPIO 18, mono, 22050 Hz (`pAPU_QUALITY` is 2 in `InfoNES_pAPU.h`). The
APU produces 8 bit unsigned, which Circle takes directly as
`SoundFormatUnsigned8`, so nothing is converted. The five channels - two pulse,
triangle, noise, DPCM - are averaged, as the reference ports do.

Volume is a percentage applied while mixing: the channels are summed and
scaled by `volume / 250`, so **50 is unity** — the plain average the reference
ports use — and is the default. 100 is twice that and clips the loudest
passages. A single NES channel only reaches a fifth of full scale, which is why
unity is quiet to begin with. Muting is separate state, so the level survives
it; `GetVolume()` returns 0 while muted and nothing downstream knows.

`CPWMSoundBaseDevice` with the queue API: `AllocateQueue`, `SetWriteFormat`,
`Start`, then `Write` per frame. 100 ms of queue. Anything that does not fit is
dropped rather than waited for, so audio never holds up a frame.

**Two traps, either of which gives perfect silence with everything apparently
working:**

- `InfoNES_GetSoundBufferSize()` must return **room left**, not data queued.
  The APU clamps how much it generates to it
  (`InfoNES_pAPUHsync` -> `std::min(bufferLeft, n)`), so returning Circle's
  `GetQueueFramesAvail()` - which is frames *waiting to be sent*, despite the
  name - deadlocks: an empty queue reads as no room, so nothing is generated,
  so the queue stays empty. Use
  `GetQueueSizeFrames() - GetQueueFramesAvail()`.
- **`APU_Mute` starts at 1** (`InfoNES.cpp:261`) and the platform layer owns
  clearing it; the pico build does it in `main.cpp`. While set,
  `InfoNES_pAPUHsync()` zeroes all five wave buffers and `K6502_rw.h:386` drops
  APU register writes. `InfoNES_SoundOpen()` clears it here.

If sound ever goes quiet, the fastest split is a test tone straight to
`CKernel::SoundOpen`/`SoundWrite` before the emulator starts: audible means the
Circle half is fine and the emulator is not feeding it.

### Frame pacing

The menu's status line reports the frame rate the **last game** achieved,
snapshotted when the menu opens - the menu is paced by the same code, so a live
reading would just measure the menu. Expect 60 or 61: the target is 60.0988 Hz
and counting whole frames in a one second window lands either side of it.

InfoNES has no PAL support at all, so the target is always NTSC. A PAL ROM will
genuinely run about 20% fast, and that is the emulator, not the pacing.

`InfoNES_LoadFrame()` presents and then waits, in that order - the frame is
going out over DMA during the wait, so the transfer costs nothing extra while
it fits in the period.

The period is 16639 us, NTSC's 60.0988 Hz, not the flat 16666 the pico build
uses. The deadline is carried forward rather than set from whenever the wait
ended, which is what that build does and what makes it drift. A frame that
overruns writes the lost time off instead of making it up: catching up means
sprinting through the frames after it, which looks worse than one late frame.

### ROM menu

`RomMenu.{h,cpp}`. Lists the `.nes` files in `/nes` on the card, Up and Down to
move, A or START to launch, SELECT + START to quit back to it.

Drawn with `C2DGraphics`, which comes free: `ST7789DMADisplay` is a `CDisplay`,
so `C2DGraphics` attaches straight to it and brings `DrawText` and Circle's
built-in fonts along. No font assets, nothing extra on the card. The menu uses
the whole 320x240; the emulator keeps handing its 256x240 frame to the display
directly, and the two never draw at once.

**The loop belongs to InfoNES, not to the kernel.** `InfoNES_Main()` runs
`InfoNES_Menu()` then `InfoNES_Cycle()`, over and over; `PAD_SYS_QUIT` in
`InfoNES_PadState()` unwinds out of the cycle (`InfoNES.cpp:864`, checked once
a scanline) and lands back in the menu. So the ROM picker *is*
`InfoNES_Menu()`, and `InfoNES_Load()` - which releases the old ROM, reads the
new one and resets - is all the teardown a game change needs. An earlier
version drove this from `CKernel::Run()` instead, which meant quitting would
have restarted the same ROM.

The menu blacks out the whole panel before returning. That matters when
`NES_FILL_WIDTH` is 0 and the emulator only writes the 256 wide strip in the
middle - the menu's background would otherwise stay in the bars either side for
the whole game.

Presses are edge-detected against a snapshot taken when the menu opens: at
60 Hz a held button runs through the list in well under a second, and the quit
chord means SELECT and START are usually still down on the way in.

### Underruns must pad with the level we output

The mix is offset so that **silence is 128, not 0**:

```c
int nValue = 128 + (nSum * nVolume) / 500;
```

This is not cosmetic. The APU's silence is 0, which is 0% PWM duty - the bottom
rail. Circle pads an underrun with its null frame, and for PWM that is half
scale (`CSoundBaseDevice` fills it with `m_nRangeMax / 2`, and the hardware
format is `SoundFormatUnsigned32` with range 0..m_nRange-1). With silence at 0,
every underrun stepped the output half of full scale and back - a click, sixty
times a second, heard as a steady chug under otherwise correct music.

Two things about that symptom are worth remembering, because they misdirect:

- **Muting made it no better.** Muting drives the samples to 0, the value
  *furthest* from the null frame, so it made each step larger. That ruled out
  the sample data as the source and sent the search towards electrical coupling
  from the display's SPI bursts, which was wrong.
- **circle-arcade does not have the problem**, with the same board, the same
  GPIO 18 and the same three PWM options. It uses `CPWMSoundDevice` for one-shot
  buffers, so it is never streaming and never underruns. The difference is the
  class, not the configuration - which is what finally pointed at the null
  frame.

Centring costs half the dynamic range; the divisor of 500 rather than 250 keeps
volume 50 at the level the reference ports produce, and the volume control makes
the rest back. Muting is now genuinely silent, since 128 is exactly the null
level.

`SKIP_DISPLAY` in `DisplayConfig.h` remains from that search: it runs the
emulator and its sound but never sends a frame, which separates anything
audible into "follows the display" and "does not".

### USB transfer mode

Holding **Up at power-on** boots into USB mass storage instead of the emulator:
the SD card appears as a drive on an attached PC. `CKernel::UpHeldAtBoot()`
reads the pin once after the pull-ups settle, and `RunUSBGadget()` takes over.

Deliberately stateless - nothing is written and no flag survives a boot, so a
plain power cycle is always an emulator again and the device cannot be stranded
in transfer mode. The file system is never mounted in this mode either, so the
two sides never hold the same FAT.

Three things Circle's gadget support requires, each of which failed silently
when missing:

- **A valid USB vendor ID.** Circle ships `USB_GADGET_VENDOR_ID` as `0x0000`,
  which `CDWUSBGadget::Initialize()` rejects outright, so a gadget with the
  default never starts. `USB_GADGET_VID`/`_PID` in `InputConfig.h` pass
  `0x1209:0x0001` (pid.codes, the open-source VID) explicitly.
- **The gadget must never be destroyed** - `~CUSBMSDGadget()` is `assert(0)`.
  It is heap allocated and leaked on purpose; the only way out is a reboot.
- **Wait for the async frame before touching USB.** `SetArea` returns while the
  DMA is still running, so halting or reconfiguring mid-transfer leaves half a
  screen - which is what a failed gadget start looked like before the wait was
  added.

`CUSBMSDGadget::Update()` does the actual block I/O rather than just pumping
state, so it cannot be called from the 16 ms game loop - which is the other
reason this is a separate boot mode rather than something offered mid-game.

Links `lib/usb/gadget/libusbgadget.a` and `lib/usb/libusb.a`. `MachineModelZeroW`
is on the gadget-capable whitelist in `dwusbgadget.cpp`.

### Not done yet

- Nothing outstanding. Possible next steps: save states, an on-screen volume
  indicator (X and Y are the only spare buttons), or per-game battery-backed
  SRAM.
