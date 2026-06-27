/* Read-only FatFs disk-I/O backend for a FAT32 image stored in XIP flash.
 *
 * The image is just a raw FAT32 disk image (produced by mkfs.fat / mtools) that
 * has been flashed to FLASHFS_BASE_ADDR. Sectors are 512 bytes; disk_read is a
 * straight memcpy from the XIP-mapped flash. There is no write path — write
 * attempts return RES_WRPRT. NES save-game state is independent (it lives in
 * dedicated flash sectors below NES_FILE_ADDR via flash_range_erase/program),
 * so a read-only ROM filesystem does not lose save functionality.
 *
 * The macros FLASHFS_BASE_ADDR and FLASHFS_SIZE_BYTES are supplied by CMake.
 */

#include "flashfs.h"

#include <string.h>
#include "pico/stdlib.h"

#ifndef FLASHFS_BASE_ADDR
#error "FLASHFS_BASE_ADDR must be defined (XIP address of the FAT image)."
#endif
#ifndef FLASHFS_SIZE_BYTES
#error "FLASHFS_SIZE_BYTES must be defined (size of the FAT image in bytes)."
#endif

#define FLASHFS_SECTOR_SIZE 512u
#define FLASHFS_SECTOR_COUNT ((LBA_t)(FLASHFS_SIZE_BYTES / FLASHFS_SECTOR_SIZE))

static DSTATUS flashfs_stat = STA_NOINIT;

static inline const uint8_t *flashfs_image(void) {
    return (const uint8_t *)(uintptr_t)FLASHFS_BASE_ADDR;
}

bool flashfs_image_present(void) {
    const uint8_t *img = flashfs_image();
    /* FAT boot-sector signature: 0x55 at offset 510, 0xAA at offset 511.
     * Quick sanity check before mounting — picotool/mkfs.fat both produce this. */
    return img[510] == 0x55 && img[511] == 0xAA;
}

DSTATUS flashfs_disk_initialize(void) {
    if (!flashfs_image_present()) {
        flashfs_stat = STA_NODISK;
        return flashfs_stat;
    }
    flashfs_stat = 0;
    return flashfs_stat;
}

DSTATUS flashfs_disk_status(void) {
    return flashfs_stat;
}

DRESULT flashfs_disk_read(BYTE *buff, LBA_t sector, UINT count) {
    if (flashfs_stat & STA_NOINIT) return RES_NOTRDY;
    if ((LBA_t)(sector + count) > FLASHFS_SECTOR_COUNT) return RES_PARERR;
    memcpy(buff,
           flashfs_image() + (size_t)sector * FLASHFS_SECTOR_SIZE,
           (size_t)count * FLASHFS_SECTOR_SIZE);
    return RES_OK;
}

DRESULT flashfs_disk_ioctl(BYTE cmd, void *buff) {
    if (flashfs_stat & STA_NOINIT) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = FLASHFS_SECTOR_COUNT;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = FLASHFS_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            /* Erase block size in sectors — unused on read-only media. */
            *(DWORD *)buff = 1;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
