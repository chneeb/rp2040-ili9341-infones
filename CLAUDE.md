# rp2040-ili9341-infones — Claude Notes

## Project Overview
RP2350-based handheld NES emulator using InfoNES, driving an ST7789 (or ILI9341) LCD via overclocked SPI. Despite the repo name referencing RP2040/ILI9341, the actual hardware is an RP2350 with an ST7789 display.

## Key Files
- `software/infones/main.cpp` — main application: display init, DMA rendering loop, InfoNES callbacks, controller input
- `software/infones/CMakeLists.txt` — build config; selects LCD controller, defines all pin assignments
- `software/infones/drivers/sdcard/sdcard.c` — SD card driver (shared SPI bus handling)

## Hardware Configuration (Current)

### MCU & Clocks
- **MCU**: RP2350 (Cortex-M33)
- **CPU clock**: 300 MHz (`CPUFreqKHz = 300000`)
- **ST7789 SPI clock**: 80 MHz (`DISPLAY_SPI_CLOCK_SPEED_HZ 80000000`)
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
- NES outputs 256×240 pixels; display is 320×240 (landscape via MADCTL MV=1, BGR=1)
- Image is scaled 256→320 (nearest-neighbor, in-place right-to-left) and fills the full width; 4px border top/bottom
- Window: columns 0–319, rows 4–235 (232 rows)
- ST7789 DISPLAY_ADDRESS_MODE = `DCS_ADDRESS_MODE_BGR | DCS_ADDRESS_MODE_SWAP_XY | DCS_ADDRESS_MODE_MIRROR_Y` (0xA8)
- ILI9341 DISPLAY_ADDRESS_MODE = `DCS_ADDRESS_MODE_BGR | DCS_ADDRESS_MODE_SWAP_XY` (0x28)
- ST7789 uses `DISPLAY_INVERT` define (set in CMakeLists.txt)

### Scanline DMA Flow
- Two ping-pong buffers: `scanline_buf_internal_1` / `scanline_buf_internal_2` (WORD[320])
- InfoNES renders scanlines 4–235 only (232 scanlines), controlled by `PPU_Scanline >= 4 && PPU_Scanline < 236` in `InfoNES.cpp`
- `InfoNES_PreDrawLine(line)` — sets InfoNES line buffer (buf1 for even, buf2 for odd lines)
- `InfoNES_PostDrawLine(line)` — scales 256→320 in-place, waits for previous DMA, then starts DMA for current line
- DMA: `DMA_SIZE_8`, 640 bytes per scanline (320 pixels × 2 bytes), DREQ-paced to SPI TX FIFO
- 232 scanlines × 320 pixels = 74,240 pixels per frame
- At 80 MHz SPI: ~14.8 ms per frame → ceiling of ~67 fps (sufficient for NES 60 fps)

### Frame Rate
- `speed_control()` in `InfoNES_LoadFrame()` caps at 60 fps (waits if frame finishes early)
- No display frame skipping is currently active (all frames DMA'd to display)
- The 60 fps cap + 80 MHz SPI + 300 MHz RP2350 achieves correct NES game speed

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

### Dead Code: ILI9341 per-frame reset (line == 0)
The ILI9341 PostDrawLine has `if (line == 0)` but `line` is always 4–235, so this block never executes.

### SD Card Init Bypassed
`initSDCard()` is never called in `main()`. `isFatalError = true` is set unconditionally, so the watchdog-reboot SD ROM loading path is permanently skipped. The device uses a single ROM flashed directly to flash at `0x10080000`. The SD card hardware fixes (CS/MISO) are in place but untested.

## Build
```bash
mkdir software/infones/build && cd software/infones/build
cmake ..
make
# Flash infoNES.uf2 to the RP2350
# Load ROM via picotool:
picotool load path/to/game.nes -t bin -o 0x10080000
```

## LCD Controller Selection (CMakeLists.txt)
```cmake
#set(LCD_CONTROLLER "ILI9341" ...)   # use ILI9341
set(LCD_CONTROLLER "ST7789" ...)     # use ST7789 (current default)
```
After switching, clear the build folder before rebuilding.
