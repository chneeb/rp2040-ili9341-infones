/*
 * Custom Pico SDK board file for the Waveshare RP2350-PiZero.
 * Not part of the official Pico SDK board list. Selected via
 *   cmake -DPICO_BOARD=waveshare_rp2350_pizero ...
 *
 * The critical setting is PICO_RP2350A 0 — the board uses the RP2350B variant
 * (48 GPIOs), so the onboard SD card on GP30/31/40/43 must be addressable.
 * Building against the official `pico2` board file silently caps NUM_BANK0_GPIOS
 * at 30 and accesses to GP30+ scribble over unrelated MMIO registers, which
 * hangs SPI1 mid-transaction (visible as "Mounting SDcard" never returning).
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_WAVESHARE_RP2350_PIZERO_H
#define _BOARDS_WAVESHARE_RP2350_PIZERO_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define WAVESHARE_RP2350_PIZERO

// --- RP2350 VARIANT ---
// RP2350B — 48 GPIOs. SD card on GP30/31/40/43 requires this.
#define PICO_RP2350A 0

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// Waveshare hasn't published a fixed user-LED pin in board notes; pick a value
// to satisfy code that reads PICO_DEFAULT_LED_PIN. infoNES re-initialises this
// pin in display_init() if it overlaps with hardware (GP25 = LCD_DC on GAMEPI20).
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

// --- FLASH ---
// 16 MB Winbond W25Q128 (same family as W25Q080 boot stage 2).
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- RP2350 silicon stepping ---
pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
