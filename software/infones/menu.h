#ifndef ROMSELECT
#define ROMSELECT
#define ROMINFOFILE "/currentloadedrom.txt"
void RomSelect_SetLineBuffer(WORD *p, WORD size);
void menu(uintptr_t NES_FILE_ADDR, char *errorMessage, bool isFatalError);

/* Defined in main.cpp. True when ROMs are served from the read-only flash FAT
 * image (drivers/flashfs); menu.cpp uses this to skip ROMINFOFILE writes and
 * the watchdog reboot, and to set romName directly so the main loop can run
 * InfoNES_Main without going through the post-reboot ROMINFOFILE read. */
extern bool flashFsActive;
extern char *romName;

#endif