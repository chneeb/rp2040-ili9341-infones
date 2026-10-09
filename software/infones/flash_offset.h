#ifndef FLASH_OFFSET_H
#define FLASH_OFFSET_H

#include <stdint.h>
#include "pico.h"
#include "hardware/regs/addressmap.h"
#if PICO_RP2350
#include "hardware/structs/qmi.h"
#endif

/* Flash offset to pass to flash_range_erase/program so the data shows up at
 * xip_addr.
 *
 * Reads through XIP_BASE go through the RP2350's QMI address translation,
 * flash_range_* takes physical offsets. The two only match while the image
 * runs from the start of flash. A bootloader that starts the image from a
 * partition (e.g. uf2loader, which keeps itself in the first 8K) maps the
 * partition to XIP_BASE, so writing to xip_addr - XIP_BASE would land below
 * where the data is read back. The RP2040 has no translation. */
static inline uintptr_t flash_offset_of(uintptr_t xip_addr)
{
    uintptr_t virt = xip_addr - XIP_BASE;
#if PICO_RP2350
    /* Each ATRANS register maps a 4MB window of the XIP address space */
    uint32_t reg = qmi_hw->atrans[virt >> 22];
    uintptr_t base = ((reg & QMI_ATRANS0_BASE_BITS) >> QMI_ATRANS0_BASE_LSB) * 4096u;
    return base + (virt & 0x3fffffu);
#else
    return virt;
#endif
}

#endif
