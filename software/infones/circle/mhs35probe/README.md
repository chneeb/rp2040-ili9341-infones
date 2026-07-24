# MHS35 / ILI9486 bring-up probe

A standalone, throwaway Circle app for bringing up the goodtft MHS35 (ILI9486,
480x320) panel on a Pi 2B/3B, separate from the emulator. It shares nothing with
the port but the display driver, which it compiles in from one directory up
(`../ILI9486DMADisplay.o`), so it exercises the real driver class.

It went through several forms during bring-up — a register-dump diagnostic that
wrote `DIAG.TXT` to the card (there is no UART, the HAT covers the header, and
the LCD has no MISO to read back), a polled colour-bar test that found the panel
needs a **16-bit register interface** (`regwidth=16`; see the port's
`ILI9486DMADisplay`), and finally a DMA test. Its current form drives
`CILI9486DMADisplay` and draws a labelled colour-quadrant test image scaled to
480x320, confirming the DMA path, orientation and scaling.

## Build and run

Circle must be configured `RASPPI=2` first (`./configure -r 2 -f` in `circle/`,
then `./makeall`). Then:

```sh
make            # -> kernel7.img
```

SD card (MBR + FAT32 — a GPT card will not boot a Pi 3B): the Circle boot files
(`bootcode.bin`, `start.elf`, `fixup.dat`), `config.txt` (a plain
`config32.txt`), and `kernel7.img`. `flash-probe-sd.sh` prepares a card from
scratch (it refuses anything that is not a removable USB disk).

The ACT LED blinks as a heartbeat, so a blank panel can be told from a crash.
