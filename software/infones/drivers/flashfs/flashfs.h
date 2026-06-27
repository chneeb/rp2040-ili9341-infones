/* Read-only FatFs disk-I/O backend for a FAT32 image stored in XIP flash.
 * Compiled only when FLASHFS_ENABLED is defined (currently GAMEPI20).
 * The dispatcher in drivers/sdcard/sdcard.c routes FatFs drive 1 here. */
#pragma once

#include <stdbool.h>
#include "ff.h"
#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sanity-check the FAT image signature at FLASHFS_BASE_ADDR.
 * Returns true if the image looks like a FAT volume (boot-sector 0x55AA at offset 510). */
bool flashfs_image_present(void);

/* diskio entry points — called by the drive-1 branch of disk_*(). */
DSTATUS flashfs_disk_initialize(void);
DSTATUS flashfs_disk_status(void);
DRESULT flashfs_disk_read(BYTE *buff, LBA_t sector, UINT count);
DRESULT flashfs_disk_ioctl(BYTE cmd, void *buff);

#ifdef __cplusplus
}
#endif
