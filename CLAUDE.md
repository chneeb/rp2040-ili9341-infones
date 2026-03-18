# rp2040-ili9341-infones — Claude Notes

## Project Overview
RP2350-based handheld NES emulator using InfoNES, driving an ST7789 (or ILI9341) LCD via overclocked SPI. Despite the repo name referencing RP2040/ILI9341, the actual hardware is an RP2350 with an ST7789 display.

## Key Files
- `software/infones/main.cpp` — main application: display init, DMA rendering loop, InfoNES callbacks, controller input
- `software/infones/CMakeLists.txt` — build config; selects LCD controller, defines all pin assignments
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

### Shared SPI Bus (CRITICAL)
The SD card and LCD share **the same SPI bus (spi1)** with CLK/MOSI/MISO on GPIO 10/11/12. They use different CS pins (LCD CS=9, SD CS=22). If display CS is LOW while the SD card is accessed, the display receives SD card SPI traffic as pixel data, corrupting the write pointer.

**SD CS early init**: GPIO 22 is driven HIGH in `main()` before `display_init()` to prevent the SD card (which retains SPI mode across watchdog reboots) from receiving display init traffic on the shared bus.

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

### SD Card Init Bypassed
`initSDCard()` is never called in `main()`. `isFatalError = true` is set unconditionally, so the watchdog-reboot SD ROM loading path is permanently skipped. The device uses a single ROM flashed directly to flash at `0x10080000`. The SD card hardware fixes (CS/MISO) are in place but untested.

## Build
```bash
mkdir software/infones/build && cd software/infones/build
cmake .. -DPICO_BOARD=pico2              # RP2350 (default)
# cmake .. -DPICO_BOARD=pico            # RP2040
make
# Flash infoNES.uf2 to the board
# Load ROM via picotool:
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
