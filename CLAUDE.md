# rp2040-ili9341-infones — Claude Notes

## Project Overview
RP2350-based handheld NES emulator using InfoNES, driving an ST7789 (or ILI9341) LCD via overclocked SPI. Despite the repo name referencing RP2040/ILI9341, the actual hardware is an RP2350 with an ST7789 display.

**There are two ports in this repository, and they target the same handheld.**
The Waveshare GamePi20 is a case: a panel, a d-pad and six buttons around a
Pi-Zero-shaped hole. Either mainboard can go in it, and there is a port for
each.

| | mainboard | build | platform layer |
|---|---|---|---|
| pico-sdk port | Waveshare RP2350-PiZero | CMake, `HARDWARE_TARGET=GAMEPI20` | `software/infones/main.cpp` |
| Circle port | Raspberry Pi Zero (W) | Make, bare metal | `software/infones/circle/` |

Everything below describes the pico-sdk port unless it says otherwise; the
Circle one has [its own section](#the-circle--gamepi20-port). The pico-sdk port
also covers three other boards that have nothing to do with the GamePi20 — see
[Hardware Target Selection](#hardware-target-selection-cmakeliststxt).

They share the emulator core and the 137 mappers untouched; only the platform
layer differs. CMake never looks at `circle/` and the Circle Makefile reaches
the shared sources by relative path, so the two builds coexist in one tree and
`git fetch upstream` still merges.

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
cmake .. -DPICO_BOARD=pico2              # RP2350 (default; pico2 = RP2350A, 30 GPIOs)
# cmake .. -DPICO_BOARD=pico            # RP2040
make
# Flash infoNES.uf2 to the board
```

**GAMEPI20 must use the in-tree `waveshare_rp2350_pizero` board file** (RP2350B, 48 GPIOs):
```bash
mkdir software/infones/build-gamepi20 && cd software/infones/build-gamepi20
cmake .. -DPICO_BOARD=waveshare_rp2350_pizero -DHARDWARE_TARGET=GAMEPI20
make
```
The `pico2` board file sets `PICO_RP2350A=1` → `NUM_BANK0_GPIOS=30`. The Waveshare RP2350-PiZero is RP2350**B** and the SD card sits on GP30/31/40/43 — addressing those pins with the wrong board file silently scribbles over unrelated MMIO registers and hangs SPI1 mid-transaction (symptom: "Mounting SDcard" never returns). The custom board file under `software/infones/boards/waveshare_rp2350_pizero.h` overrides `PICO_RP2350A=0` and sets sensible defaults; CMakeLists.txt prepends `boards/` to `PICO_BOARD_HEADER_DIRS` so the SDK finds it.

**ROM loading (PICO_RESTOUCH / ORIGINAL_RP2040):** Copy `.nes` files to an SD card (FAT32). On first boot the file-browser menu appears; select a game to play. The selection is saved to `/currentloadedrom.txt` and the device reboots into the emulator. The menu reappears after the game exits.

**ROM loading (WAVESHARE_LCD13 / no SD card):** Flash a single ROM directly to flash with picotool:
```bash
picotool load path/to/game.nes -t bin -o 0x10080000
```

**ROM loading (GAMEPI20):** Same SD card flow as PICO_RESTOUCH — the RP2350-PiZero has an onboard SD slot. Copy `.nes` files to FAT32, pick from the menu, and the device reboots into the emulator.

`CPU_FREQ_KHZ` is set automatically from the cmake `PICO_RP2350` variable (300 MHz for RP2350, 252 MHz for RP2040) — it is not part of `HARDWARE_TARGET`.

## Hardware Target Selection (CMakeLists.txt)

Set `HARDWARE_TARGET` via `-DHARDWARE_TARGET=...` (or edit the default in CMakeLists.txt).

**Use a separate build directory per target/board.** e.g. `build-pico-restouch/`, `build-gamepi20/`, `build-waveshare-lcd13/`. CMake re-uses cached state per directory, so switching targets is just `cd build-foo && make` — no re-download of the SDK, no recompile of TinyUSB/Pico SDK objects. Wiping a build dir is only necessary when changing something CMake can't pick up incrementally (e.g. editing a board header file in place, switching `PICO_PLATFORM` within the same dir).

`HARDWARE_TARGET` describes peripheral wiring only (pins, display type, controller). Board/chip is selected separately via `-DPICO_BOARD`.

### Configuration comparison

| Aspect | ORIGINAL_RP2040 | PICO_RESTOUCH | WAVESHARE_LCD13 | GAMEPI20 |
|---|---|---|---|---|
| Tested MCU | RP2040 | RP2350 | RP2350 | RP2350 (Waveshare RP2350-PiZero) |
| LCD | ILI9341 320×240 | ST7789 320×240 | ST7789 240×240 | ILI9341 320×240 (GamePi20 HAT) |
| LCD bus | hardware spi0 | hardware spi1 | hardware spi1 | hardware spi1 |
| LCD DC/CS/CLK/MOSI | 20/17/18/19 | 8/9/10/11 | 8/9/10/11 | 25/8/10/11 |
| LCD RST / BL | 21 / 22 | 15 / 13 | 12 / 13 | 27 / 24 |
| SD SPI bus | spi1 (separate) | spi1 (shared, same pins) | none | spi1 (shared, separate pins) |
| SD pins SCK/MOSI/MISO/CS | 10/11/12/13 | 10/11/12/22 | — | 30/31/40/43 (RP2350-PiZero onboard) |
| Touch CS | none | GP16 | none | none |
| Controller | none | NES Mini (i2c1, GP26/27) | GPIO buttons+joystick | GPIO buttons+D-pad |
| CPU clock | from chip (252/300 MHz) | from chip (252/300 MHz) | from chip (252/300 MHz) | from chip (252/300 MHz) |
| VREG | no | VREG_VOLTAGE_1_20 | VREG_VOLTAGE_1_20 | VREG_VOLTAGE_1_20 |
| `SHARED_SPI_BUS` | — | ✓ | — | ✓ |
| NES scanlines rendered | 4–235 (232 rows) | 4–235 (232 rows) | 0–239 (240 rows) | 4–235 (232 rows) |

### Waveshare RP2350-PiZero header GPIO mapping

The board is **not** a straight GP=BCM mapping. Waveshare swapped pins so the RP2350's hardware **SPI1** and **UART1** land at the Pi-standard SPI0 / UART0 positions on the 40-pin header. Verified from the silkscreen on the back of the board:

| Swap pair | Pi role on header | Pi BCM | RP2350 GP | RP2350 function |
|---|---|---|---|---|
| SPI MOSI/MISO/SCK | pin 19 (MOSI) | BCM10 | **GP11** | SPI1_TX |
| | pin 23 (SCLK) | BCM11 | **GP10** | SPI1_SCK |
| | pin 21 (MISO) | BCM9 | **GP12** | SPI1_RX |
| UART TX/RX | pin 8 (TX) | BCM14 | **GP4** | UART1_TX |
| | pin 10 (RX) | BCM15 | **GP5** | UART1_RX |
| (displaced) | pin 7 | BCM4 | **GP14** | — |
| (displaced) | pin 29 | BCM5 | **GP15** | — |
| (displaced) | pin 32 | BCM12 | **GP9** | — |

All other header GPIOs map GP*n* = BCM*n*.

**Practical implications for GAMEPI20:**
- The LCD uses hardware SPI1: `LCD_CLK=10`, `LCD_MOSI=11` — these land at pins 23/19 (BCM11/BCM10 = `LCD_SCK`/`LCD_MOSI` on the GamePi20). Hardware SPI works without jumpers because of the board's internal swap.
- The GamePi20 D-pad UP (BCM12, pin 32) lands on **GP9**, not GP12. Earlier the firmware used `JOY_UP=12` (assuming GP=BCM) — pressing UP did nothing because GP12 is actually at pin 21 (BCM9 / MISO position).
- B button (BCM4, pin 7) → **GP14**. TL shoulder (BCM5, pin 29) → **GP15**.
- An even earlier iteration drove the LCD via PIO with MOSI=GP10/SCK=GP11. On this board that routes MOSI to pin 23 (LCD_SCK) and SCK to pin 19 (LCD_MOSI) — the LCD never receives framed data, screen stays dark with backlight on. The fix is just to use hardware SPI1 with `LCD_CLK=10, LCD_MOSI=11`.
- If a future Pi-Zero-style RP2350 board uses a different mapping, the corresponding firmware changes are confined to the GPIO numbers in CMakeLists.txt — no driver changes needed.

### GAMEPI20 SD card (RP2350-PiZero onboard slot)

Hardware SPI1, SCK=GP30, MOSI=GP31, MISO=GP40, CS=GP43 — RP2350B extended GPIOs, internally wired on the board (do not pass through the 40-pin header).

Both the LCD (via GP10/11) and the SD card (via GP30/31/40) live on the *same* SPI1 peripheral. Configuring SPI1 with both pin sets means SPI1 drives them simultaneously (GP10 and GP30 both clock; GP11 and GP31 both carry MOSI data), with CS lines keeping the two devices separated. This is the "shared SPI1, separate physical wires" pattern — `SHARED_SPI_BUS` workarounds still apply (LCD CS HIGH during SD ops, SD CS HIGH at boot, baudrate restore after SD ops).

`sdcard.c`'s `init_spi()` calls `gpio_set_function(SCK/MOSI, GPIO_FUNC_SPI)` unconditionally. For PICO_RESTOUCH this is a harmless no-op (display init already set those same pins). For GAMEPI20 it's required because the LCD's SPI init configures GP10/11, not GP30/31.

### No-SD-card ROM library: TAR in the single-ROM region (working)

When no SD card is inserted, the firmware falls through to its "single-ROM" path: `RomSelector::init(NES_FILE_ADDR)` (rom_selector.h) inspects the bytes at `0x10080000`. If they start with `NES\x1A` it's treated as a raw `.nes` ROM; otherwise it's parsed as a **POSIX ustar archive** (`tar.cpp:parseTAR()`) and each `.nes` entry inside becomes a selectable ROM. The TAR header magic check is strict — `strcmp("ustar", header+257) == 0` — so the archive must be built with `tar --format=ustar` (GNU tar's default `--format=gnu` adds PaxHeader records that fail the check).

Building and flashing a TAR of ROMs:

```bash
tools/mkromtar.sh ~/roms/nes/ roms.tar                 # ustar archive of *.nes (case-insens.)
picotool load -F roms.tar -t bin -o 0x10080000 --family absolute
picotool reboot
```

The script forces `--format=ustar`, verifies the magic at offset 257, copies only top-level `.nes` files (case-insensitive — `.NES`, `.Nes`, `.nes`), and warns if the archive exceeds the 1 MB region between `NES_FILE_ADDR` and the next reserved area at `0x10180000`. ~15–25 typical NES ROMs fit comfortably.

**In-game ROM switching**: hold SELECT + tap LEFT to cycle to the previous ROM in the archive; SELECT + RIGHT for the next. `RomSelector::prev()`/`next()` (rom_selector.h) advance the in-archive index; the input handler in `main.cpp:InfoNES_PadState()` calls `saveNVRAM()` then sets `reset = true` so `InfoNES_Main()` exits cleanly; the outer loop re-runs it against the now-current ROM (via the `continue` in the fatal-error branch of `main()`). For single-ROM mode these calls are no-ops (`prev()`/`next()` early-return when `singleROM_` is set).

### GAMEPI20 flash-resident FAT32 fallback (DISABLED — picotool flash issue unresolved)

> Status: code in tree (`FLASHFS_ENABLED`-gated), default for GAMEPI20 enables it via `add_compile_definitions(FLASHFS_ENABLED)` in CMakeLists.txt. Currently *does not work end-to-end*: `picotool load -F romdisk.img -t bin -o 0x10200000 --family absolute` reports success and `picotool save` confirms the boot sector lands at `0x10200000` (passes the `flashfs_image_present()` 0x55AA preflight), but FatFs's `f_mount` returns `FR_NO_FILESYSTEM` (13). The image itself is valid — Linux's vfat driver mounts it fine. Root cause still TBD; suspect a picotool flash quirk on RP2350. Working alternative for now is the TAR path above.
>
> To disable the path entirely, delete `add_compile_definitions(FLASHFS_ENABLED)` (plus the two FLASHFS_* address defines) from the GAMEPI20 elseif in CMakeLists.txt; the rest compiles out.

When working, this mounts a **read-only** FAT32 image stored directly in XIP flash and serves ROMs from it. Implementation lives in `drivers/flashfs/` (`flashfs.c` + `flashfs.h`); the dispatcher in `drivers/sdcard/sdcard.c` routes FatFs drive 1 to it when `FLASHFS_ENABLED` is set. `FF_VOLUMES=2` in `ffconf.h` (drive 0 = SD, drive 1 = flash).

Flash layout (16 MB chip):
- `0x10000000+` — firmware (~290 KB observed; leaves room to grow)
- (grows downward to `0x10080000`) — NES SRAM save slots (`flash_range_erase`/`program`; one sector per ROM slot, indexed via `RomSelector`)
- `0x10080000` — `NES_FILE_ADDR`, the 1 MB single-ROM (or ustar TAR) region
- `0x10180000–0x101FFFFF` — reserved gap (512 KB)
- **`0x10200000` — `FLASHFS_BASE_ADDR`, 14 MB FAT32 image (disabled in practice)**
- `0x11000000` — end of flash

Save games are *not* in the FAT image — they continue to live in the dedicated flash sectors below `NES_FILE_ADDR`. The FAT image only carries ROMs, so it can stay read-only without breaking saves.

Boot sequence (GAMEPI20):
1. `main()` calls `initSDCard()` first. If the card mounts, behaviour is unchanged (drive 0, ROMINFOFILE/watchdog dance, etc.).
2. If SD fails and `FLASHFS_ENABLED`, `initFlashFS()` runs: preflights the 0x55AA boot signature, then `f_mount(&fsFlash, "1:", 1)` + `f_chdrive("1:")`. Sets the global `flashFsActive = true` on success.
3. If both fail, `isFatalError = true` → single-ROM/TAR fallback at `NES_FILE_ADDR` (which is where the TAR path lives).

Menu behaviour when `flashFsActive`:
- The ROM-copy step (`f_read` then `flash_range_erase`/`program` into `NES_FILE_ADDR`) runs unchanged — all reads on the FAT side.
- The `ROMINFOFILE` write is skipped (read-only filesystem).
- The post-selection `watchdog_enable()` is skipped; `menu()` instead copies the basename into `romName` and returns. The outer loop in `main()` runs `InfoNES_Main()` against the just-flashed ROM at `NES_FILE_ADDR`. On player quit, the loop empties `selectedRom` and calls `menu()` again — no reboot needed.

Tooling for the FAT image — `tools/mkromfs.sh` wraps `mkfs.fat` + `mtools`:

```bash
tools/mkromfs.sh ~/roms/nes/ romdisk.img       # build 14 MB image
picotool load -F romdisk.img -t bin -o 0x10200000 --family absolute
```

Requires `dosfstools` + `mtools` (`sudo apt install dosfstools mtools` on Debian/Ubuntu, `brew install dosfstools mtools` on macOS).

### GamePi20 button mapping

GPIOs reflect the Waveshare RP2350-PiZero's header swap (BCM↔GP not identity — see the mapping table above). NES A/B are reversed vs. the GamePi20's silkscreen so the rightmost face button registers as NES A (NES controller convention).

| NES button | GamePi20 input | BCM | GP |
|---|---|---|---|
| A | **B** button (rightmost on GamePi20 layout) | BCM4 | **GP14** |
| B | **A** button | BCM23 | GP23 |
| Select | SELECT | BCM16 | GP16 |
| Start | START | BCM26 | GP26 |
| Up | Up D-pad | BCM12 | **GP9** |
| Down | Down D-pad | BCM20 | GP20 |
| Left | Left D-pad | BCM21 | GP21 |
| Right | Right D-pad | BCM13 | GP13 |
| (quit/menu) | TL shoulder | BCM5 | **GP15** |

Bolded GPs are the ones that diverge from the naive GP=BCM assumption — those positions take their values from the RP2350-PiZero's internal header swap.

Unmapped GamePi20 buttons: X (BCM22/GP22), Y (BCM17/GP17), TR (BCM6/GP6). Extend `InfoNES_PadState()` to use them if needed.

Audio: `DISABLE_AUDIO` is currently defined for GAMEPI20 — the PWM audio output to GP18 (BCM18, header pin 12 / earphone jack) sounds wrong and is short-circuited until that's diagnosed. With the flag set, `InfoNES_SoundOutput` returns immediately and `multicore_launch_core1` is skipped (no PWM init, core1 stays idle). The pin selection itself is configurable per target via the `AUDIO_PIN` CMake variable (default GP7 preserves prior behaviour for non-GAMEPI20 targets); re-enable later by removing `add_compile_definitions(DISABLE_AUDIO)` from the GAMEPI20 elseif branch in CMakeLists.txt.

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
| GAMEPI20 (ILI9341) | `DCS_ADDRESS_MODE_RGB \| DCS_ADDRESS_MODE_SWAP_XY \| DCS_ADDRESS_MODE_MIRROR_Y` | 0xA0 |
| PICO_RESTOUCH (ST7789 320×240) | `DCS_ADDRESS_MODE_RGB \| DCS_ADDRESS_MODE_SWAP_XY \| DCS_ADDRESS_MODE_MIRROR_Y` | 0xA0 |
| WAVESHARE_LCD13 (ST7789 240×240) | `DCS_ADDRESS_MODE_MIRROR_X \| DCS_ADDRESS_MODE_SWAP_XY` | 0x60 |

GAMEPI20 also defines `DISPLAY_INVERT` (sent as `DCS_ENTER_INVERT_MODE`) — the specific ILI9341 panel on the GamePi20 boots with inverted pixel polarity.

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
| `AUDIO_PIN` | PWM audio output GPIO. Per-target; default GP7. GAMEPI20 sets GP18 (earphone jack). |
| `DISABLE_AUDIO` | Short-circuit `InfoNES_SoundOutput` and skip `multicore_launch_core1` — emulator runs silent. Used by GAMEPI20 while the GP18 audio is being investigated. |
| `FLASHFS_ENABLED` | Compile and link `drivers/flashfs/`; `sdcard.c` dispatches FatFs drive 1 to it. GAMEPI20 only. |
| `FLASHFS_BASE_ADDR` / `FLASHFS_SIZE_BYTES` | XIP address and byte size of the flash-resident FAT32 image. GAMEPI20: `0x10200000` / 14 MB. |
## The Circle / GamePi20 port

In `software/infones/circle/`. The same InfoNES core, running bare metal on a
Raspberry Pi Zero (W) in a Waveshare GamePi20, through
[Circle](https://github.com/rsta2/circle) instead of the pico-sdk. No OS.

**This is the same case as the pico-sdk `GAMEPI20` target, with a Pi Zero in it
instead of a Waveshare RP2350-PiZero.** The two are unrelated codebases that
happen to drive the same buttons and the same panel; nothing is shared between
them but the emulator core. Note they configure *different display
controllers* — ST7789 here, ILI9341 there — which is worth checking against
the HAT revision in hand if either is ever brought up on the other's hardware.

Working on hardware: picture, colour, buttons, sound, frame pacing, the ROM
menu, cart battery saves and USB transfer mode. See
[Not done yet](#not-done-yet) for what is left.

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
`circle/boot`), `config.txt` (copy `boot/config.txt` from this repo, **not**
`circle/boot/config32.txt` — it is that file plus the pinned core clock),
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
| 320 px @ 62.5 MHz | 19.7 ms | every second frame - 30 Hz picture |
| 320 px @ 87.5 MHz (default) | 14.0 ms | every frame - 60 Hz picture |
| 320 px @ 100 MHz | 12.3 ms | every frame, but degrades the panel |

**The picture rate is set from `boot/config.txt`, not from a rebuild.** The bus
rate is `ST7789_CLOCK_DIVISOR` (4) into the core clock as measured once at
boot, so `core_freq` is the only knob:

| `core_freq` | bus | picture |
|---|---|---|
| 250 | 62.5 MHz | 30 Hz, gentler on the battery |
| 350 (shipped) | 87.5 MHz | 60 Hz |

Boot holding Up for USB transfer mode, edit the line, power cycle. No toolchain
involved. `ST7789_CLOCK_CEILING` caps the result at 90 MHz whatever `core_freq`
says, because an unpinned core caught boosting at boot would divide to exactly
the 100 MHz that degrades this panel.

The derivation is `CST7789DMADisplay::TargetClock()`, which reads the core
**once** and caches it — the first call is the display's own constructor
argument, before anything has had a chance to load the core and move it. A
target that drifted with the core is precisely what `SetTargetClock()` exists
to correct for, so re-reading would cancel the whole arrangement out.

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

- **`config.txt` pins the core**, and `boot/config.txt` in this repo is the
  copy to use — it has `core_freq`/`core_freq_min` already in the right place.
  **These must sit above the `[pi4]`/`[cm4]` markers.**
  Anything after a `[model]` filter applies only to that model, so a `core_freq`
  at the end of the file is silently ignored on a Zero - which is exactly what
  happened for several rounds here, and is why an earlier version of this note
  wrongly claimed the firmware ignores `core_freq` altogether. It does not; it
  was in the wrong section.
- **`CST7789DMADisplay::SetTargetClock()` re-aims the bus** once a second from
  `CKernel::WaitForNextFrame()`, holding it at `TargetClock()` whatever
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
scaled by `volume / 500`, so **50 is unity** — the plain average the reference
ports use. 100 is twice that and clips the loudest passages. A single NES
channel only reaches a fifth of full scale, which is why unity is quiet to
begin with. Muting is separate state, so the level survives it; `GetVolume()`
returns 0 while muted and nothing downstream knows.

`VOLUME_DEFAULT` is **15**, well below unity: the board drives a small speaker
straight off the PWM pin, and TL/TR step up from there. Starting low costs a
couple of presses; starting too loud cannot be taken back.

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

InfoNES has no PAL support at all, so the core always runs 262 scanlines at the
NTSC rate. The platform layer corrects most of what that costs a PAL ROM — see
[Region](#region-ntsc-and-pal) below.

`InfoNES_LoadFrame()` presents and then waits, in that order - the frame is
going out over DMA during the wait, so the transfer costs nothing extra while
it fits in the period.

The period is 16639 us, NTSC's 60.0988 Hz, not the flat 16666 the pico build
uses. The deadline is carried forward rather than set from whenever the wait
ended, which is what that build does and what makes it drift. A frame that
overruns writes the lost time off instead of making it up: catching up means
sprinting through the frames after it, which looks worse than one late frame.

### Region (NTSC and PAL)

A PAL game on NTSC timing expects 50 frames a second and gets 60.1, so it plays
about 20% fast with the music pitched up to match. Two constants in the
platform layer fix that, and the shared core is not touched:

- **Frame period 19997 µs** instead of 16639 (50.007 Hz against 60.0988).
  `GamePi20_SetFramePeriod()`, set from `InfoNES_ReadRom()`.
- **Sound device opened at 18347 Hz** instead of 22050.

The second one is not optional and is not obvious. The APU's output per second
is tied to the frame rate — it generates a fixed number of samples per scanline
(`ApuQual[]` in `InfoNES_pAPU.cpp`, 22050/60/262 of them) — so pacing at 50 Hz
produces five sixths as many samples a second. Left at 22050 that is a
permanent underrun, breaking up fifty times a second. Opening at five sixths of
the rate balances it *and* fixes the pitch in the same stroke: samples computed
for 22050 Hz played at 18347 come out a factor 0.83207 lower, against the
0.83208 a PAL game wants. One change, both problems.

`CKernel::SoundOpen()` rebuilds the device when the rate changes, since
`CPWMSoundBaseDevice` takes its rate at construction and going from an NTSC
game to a PAL one changes it.

**A happy accident:** the APU frame counter ticks per scanline, so slowing to
50 Hz puts it at 240 × 50/60 = 200 Hz, which is the real PAL rate. Envelopes
and sweeps come right for free.

**What is still wrong.** A PAL machine has 312 scanlines and correspondingly
more vblank; this still has 262. Games that do heavy work in vblank, or time
raster effects to the longer frame, can still misbehave. Fixing that means real
PAL support in the core, which would cost the untouched-upstream property.

**Detection is deliberately half-blind.** `HeaderSaysPAL()` believes only a
NES 2.0 header — byte 7 bits 2–3 == 2, then byte 12 bits 0–1, where 1 is PAL.
iNES 1.0's PAL bit at byte 9 bit 0 is ignored, because practically every dump
leaves it clear whatever the game is, so reading it would mislabel PAL ROMs as
NTSC far more often than it helped. An undetected PAL ROM behaves exactly as it
did before, which is the safe way to be wrong — but it does mean one game can
be corrected and another not, for reasons invisible from the menu. A region
column in the ROM list would earn its keep.

To check a ROM by hand: `xxd -l 16 game.nes`, then read bytes 7 and 12.

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

### Battery backed SRAM

Carts that declare a battery (`ROM_SRAM`, from the iNES header) keep their 8 KB
at `0x6000-0x7fff` in `<game>.sav` next to the ROM in `/nes`. All of it is in
`InfoNES_System_Circle.cpp`; no new files, so the Makefile is untouched.

This is the cart's own battery, **not a save state**. Zelda and Final Fantasy
keep their slots; a passwords game still uses passwords.

Loaded in `InfoNES_Menu()` *after* `InfoNES_Load()`, which is the only correct
point: `InfoNES_ReadRom()` zeroes SRAM and `InfoNES_Reset()` is what sets
`ROM_SRAM`, so neither is known before it returns.

**Flushed every 300 frames, not on quit.** The device is switched off mid-game
far more often than it is quit to the menu, so a quit-only flush would lose
most saves. Five seconds bounds the loss.

**The trigger is a memcmp against a shadow copy, not `SRAMwritten`.** That flag
is set by *any* write to the region, and plenty of games use the area as
scratch work RAM and set it every frame — flushing on it would rewrite an
identical 8 KB forever. Comparing against what is already on the card costs a
few microseconds at 0.2 Hz and writes only on a real change.

**Written to a `.tmp` and swapped in, never over the `.sav` in place.** The
flush is periodic and unannounced, so the machine is most likely to be switched
off during exactly this. Writing in place means truncating a good save and then
taking milliseconds to replace it — power off in between and everything is
gone. Building beside it makes the worst case a throwaway `.tmp` and a `.sav`
a few seconds stale.

The swap is `f_unlink` then `f_rename` (FatFs will not rename onto an existing
name), so there is still an instant with no `.sav`. It is metadata rather than
8 KB of data — microseconds against milliseconds — and `LoadSRAM()` falls back
to the `.tmp` to cover it. A file of the wrong length from either name is
rejected rather than half loaded.

The flush sits between the frame present and `GamePi20_WaitForNextFrame()`, so
it comes out of the slack already in the frame rather than adding to it, and
EMMC is a different peripheral from the panel's SPI so it does not disturb the
transfer in flight.

### Not done yet

- Save states (a full machine snapshot, as opposed to the cart battery above).
  The core has no interface for it: `A/X/Y/SP/F` are file-scope in `K6502.cpp`
  and not in `K6502.h`; `ROMBANK`/`PPUBANK`/`SRAMBANK` are raw pointers that
  have to be stored as offsets and re-derived; and each of the 137 mappers
  keeps private globals (`Map4_Regs`, `Map4_IRQ_Cnt`, …) with no common save
  hook, so it would have to be done per mapper. Mappers 0–4 cover most of the
  library. `ChrBuf` need not be saved — set `ChrBufUpdate = 0xFF` and let it
  regenerate.
- An on-screen volume indicator (X and Y are the only spare buttons).
- **Persisting the volume**, which currently resets to `VOLUME_DEFAULT` every
  boot. Designed but not written; the shape it should take:

  Kernel side, not the InfoNES platform layer — `CKernel` already owns both
  `m_nVolume` and the FATFS mount, so routing it through `GamePi20.h` would
  drag it across the bridge for nothing.

  Text `key=value` at `SD:/settings.txt`. `FF_USE_STRFUNC` is 1, so `f_printf`
  and `f_gets` are there and no parser is needed; the root of the card puts it
  beside `config.txt` for editing over USB transfer mode; and it extends to a
  last-played ROM or a brightness without a format change.

  **Flush lazily, not on change.** Holding TR from 0 to 100 is twenty steps in
  a few seconds. Mark dirty and write from the once-a-second branch already in
  `WaitForNextFrame()` (`m_nFramesThisSecond == 0`, where the clock is
  re-aimed), so it is at most one write a second and only after a real change.

  **No `.tmp` and rename, unlike the SRAM path** - deliberately. That protects
  8 KB of irreplaceable progress; this is twenty bytes recreated with two
  button presses, in a single sector, behind a one second window. Writing in
  place is right, and duplicating the crash-safe dance would be cargo cult.

  Load after the mount and before `SoundOpen()`, clamped to `VOLUME_MAX` so a
  hand-edited file cannot hand the mixer a silly number.

  Persist the volume but **not** the mute: booting silent with no on-screen
  indicator reads as broken hardware, and mute is one chord to restore.
