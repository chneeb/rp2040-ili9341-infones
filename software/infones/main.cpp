#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/divider.h"
#include <hardware/spi.h>
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"
#include "hardware/interp.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include <hardware/sync.h>
#include <pico/multicore.h>
#include <hardware/flash.h>
#include <memory>
#include <math.h>
//#include <util/dump_bin.h>
// #include <util/exclusive_proc.h>
//#include <util/work_meter.h>
#include <string.h>
#include <stdarg.h>
#include <algorithm>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"

//#include <dvi/dvi.h>
#include <tusb.h>
// #include "gamepad.h"
#include "rom_selector.h"
#include "menu.h"

#ifdef __cplusplus

#include "ff.h"

#endif

#ifdef FLASHFS_ENABLED
#include "flashfs.h"
#endif

// #include <hagl_hal.h>
// #include <hagl.h>
// #define ST7789
// #undef ILI9341

#define DCS_SOFT_RESET                 0x01
#define DCS_EXIT_SLEEP_MODE            0x11
#define DCS_EXIT_INVERT_MODE           0x20
#define DCS_ENTER_INVERT_MODE          0x21
#define DCS_SET_DISPLAY_ON             0x29
#define DCS_SET_COLUMN_ADDRESS         0x2A
#define DCS_SET_PAGE_ADDRESS           0x2B
#define DCS_WRITE_MEMORY_START         0x2C
#define DCS_SET_ADDRESS_MODE           0x36
#define DCS_SET_PIXEL_FORMAT           0x3A

#define DCS_PIXEL_FORMAT_16BIT         0x55 /* 0b01010101 */
#define DCS_PIXEL_FORMAT_8BIT          0x22 /* 0b00100010 */

#define DCS_ADDRESS_MODE_MIRROR_Y      0x80
#define DCS_ADDRESS_MODE_MIRROR_X      0x40
#define DCS_ADDRESS_MODE_SWAP_XY       0x20
#define DCS_ADDRESS_MODE_BGR           0x08
#define DCS_ADDRESS_MODE_RGB           0x00
#define DCS_ADDRESS_MODE_FLIP_X        0x02

/* DISPLAY_SPI_CLOCK_SPEED_HZ is defined via CMakeLists.txt */

#define    DISPLAY_PIXEL_FORMAT DCS_PIXEL_FORMAT_16BIT

#ifdef ILI9341
#define    DISPLAY_ADDRESS_MODE DCS_ADDRESS_MODE_BGR | DCS_ADDRESS_MODE_SWAP_XY
#endif
#ifdef ST7789
#define    DISPLAY_ADDRESS_MODE DCS_ADDRESS_MODE_RGB | DCS_ADDRESS_MODE_SWAP_XY | DCS_ADDRESS_MODE_MIRROR_Y
#endif

#ifdef HARDWARE_TARGET_WAVESHARE_LCD13
/* Waveshare Pico LCD 1.3" (ST7789 240x240) corrections:
 *   DISPLAY_ADDRESS_MODE: MX|MV (0x60) — 90° rotation with horizontal mirror for correct
 *                         landscape orientation and image direction.
 *   No GRAM offset needed: with MX+MV the MX mirror compensates for the 80-row portrait offset,
 *                          so CASET 0-239 and RASET 0-239 map directly to the full visible area. */
#undef  DISPLAY_ADDRESS_MODE
#define DISPLAY_ADDRESS_MODE DCS_ADDRESS_MODE_MIRROR_X | DCS_ADDRESS_MODE_SWAP_XY
#define    DISPLAY_OFFSET_X 0
#define    DISPLAY_OFFSET_Y 0
#elif defined(HARDWARE_TARGET_GAMEPI20)
/* GamePi20 (ILI9341) is mounted rotated 180° vs ORIGINAL_RP2040 and the panel
 * accepts pixels in RGB order (vs ORIGINAL_RP2040's BGR). The 180° rotation
 * of the X-mirror state is the Y-mirror state — using MX alone leaves the
 * image upside-down; adding MY on top of MX only flips one further axis,
 * not 180°. Final mode: RGB | MV | MY = 0xA0. */
#undef  DISPLAY_ADDRESS_MODE
#define DISPLAY_ADDRESS_MODE (DCS_ADDRESS_MODE_RGB | DCS_ADDRESS_MODE_SWAP_XY \
                              | DCS_ADDRESS_MODE_MIRROR_Y)
#define    DISPLAY_OFFSET_X 0
#define    DISPLAY_OFFSET_Y 0
#else
#define    DISPLAY_OFFSET_X 0
#define    DISPLAY_OFFSET_Y 0
#endif


// DISPLAY_WIDTH and DISPLAY_HEIGHT are provided by CMake via the HARDWARE_TARGET selection.


//#undef    DISPLAY_INVERT 


#include "audio.h"
#include "FrensHelpers.h"
#ifdef I2S_AUDIO
#include "i2s_output.h"
#ifndef I2S_GAIN_PERCENT
#define I2S_GAIN_PERCENT 150
#endif
#endif


// Controller pins and type are provided by CMake via HARDWARE_TARGET selection.
// CONTROLLER_NUNCHUCK  → NES Mini Classic clone over I2C (PICO_RESTOUCH)
// CONTROLLER_GPIO_BUTTONS → 4 buttons + joystick on GPIO (WAVESHARE_LCD13)
// (no define)          → no controller (ORIGINAL_RP2040)
#define NUNCHUCK_ADDR 0x52

#ifdef CONTROLLER_NUNCHUCK
static void nunchuck_init();
static bool nunchuck_read(uint8_t data[8]);
#endif

const uint LED_PIN = PICO_DEFAULT_LED_PIN;

// static hagl_backend_t *display;
// InfoNES always renders 256px wide.  For 320-wide targets the scaling loop writes
// fb[0..319] in-place, so the buffer must be sized to DISPLAY_WIDTH (not just 256).
// For 240-wide targets the crop loop only reads fb[8..247] and writes fb[0..239],
// so 256 elements is sufficient there.
#if DISPLAY_WIDTH > 256
#define SCANLINE_BUF_WORDS DISPLAY_WIDTH
#else
#define SCANLINE_BUF_WORDS 256
#endif
WORD scanline_buf_internal_1[SCANLINE_BUF_WORDS];
WORD scanline_buf_internal_2[SCANLINE_BUF_WORDS];
WORD scanline_buf_outgoing[SCANLINE_BUF_WORDS];
// BYTE framebuffer[256*240];
// uint8_t screen_x;
// uint8_t screen_x_start;
// uint8_t screen_y;
// bool line_drawing=false;
// BYTE frame_skip;
volatile bool SoundOutputBuilding = true;
volatile BYTE frame_skip_counter=0;
int frame_column_step=0;
// #define FRAME_COLUMN_WIDTH 28
int FRAME_COLUMN_WIDTH=28;
// #define AUDIO_BUF_SIZE 735*5
#define AUDIO_BUF_SIZE 2048
BYTE snd_buf[AUDIO_BUF_SIZE]={0};
int buf_residue_size=AUDIO_BUF_SIZE;
static int display_dma_channel;

#include "hardware/sync.h"

#define AUDIO_RING_BUFFER_SIZE 8192 // Increased buffer size

struct AudioRingBuffer {
    uint8_t buffer[AUDIO_RING_BUFFER_SIZE];
    volatile int head = 0;
    volatile int tail = 0;
    spin_lock_t *lock;
    bool initialized = false;

    void init() {
        if (!initialized) {
            lock = spin_lock_init(spin_lock_claim_unused(true));
            head = 0;
            tail = 0;
            initialized = true;
        }
    }

    int __not_in_flash_func(check_initialized)() {
        if (!initialized) init();
        return 0;
    }

    int writable_size() {
        check_initialized();
        int h = head;
        int t = tail;
        // Simple calculation: size - 1 - occupied
        int occupied;
        if (h >= t) occupied = h - t;
        else occupied = AUDIO_RING_BUFFER_SIZE - (t - h);
        
        return AUDIO_RING_BUFFER_SIZE - 1 - occupied;
    }

    int readable_size() {
        check_initialized();
        int h = head;
        int t = tail;
        if (h >= t) return h - t;
        return AUDIO_RING_BUFFER_SIZE - (t - h);
    }

    void write(const uint8_t* data, int len) {
        check_initialized();
        uint32_t saved_irq = spin_lock_blocking(lock);
        for(int i=0; i<len; ++i) {
             buffer[head] = data[i];
             head = (head + 1) % AUDIO_RING_BUFFER_SIZE;
        }
        spin_unlock(lock, saved_irq);
    }

    void reset() {
        check_initialized();
        uint32_t saved_irq = spin_lock_blocking(lock);
        head = tail = 0;
        spin_unlock(lock, saved_irq);
    }

    /* Called from core1, which must not touch flash while core0 writes it. */
    int __not_in_flash_func(read)(uint8_t* dest, int max_len) {
        check_initialized();
        uint32_t saved_irq = spin_lock_blocking(lock);
        
        // precise readable check inside lock
        int h = head;
        int t = tail;
        int count;
        if (h >= t) count = h - t;
        else count = AUDIO_RING_BUFFER_SIZE - (t - h);

        int to_read = (max_len < count) ? max_len : count;
        
        for(int i=0; i<to_read; ++i) {
            dest[i] = buffer[tail];
            tail = (tail + 1) % AUDIO_RING_BUFFER_SIZE;
        }
        spin_unlock(lock, saved_irq);
        return to_read;
    }
};

AudioRingBuffer audioRing;


// #ifndef DVICONFIG
// //#define DVICONFIG dviConfig_PicoDVI
// #define DVICONFIG dviConfig_PicoDVISock
// #endif

#define ERRORMESSAGESIZE 40
#define GAMESAVEDIR "/SAVES"
// util::ExclusiveProc exclProc_;
char *ErrorMessage;
bool isFatalError = false;
/* When true, ROM data is being served from the read-only flash FAT image
 * (drivers/flashfs). menu.cpp uses this to skip the ROMINFOFILE write and
 * the watchdog reboot — the flash image can't be written, and we don't need
 * the reboot dance because audio is disabled on the only target that uses it. */
bool flashFsActive = false;
static FATFS fs;
#ifdef FLASHFS_ENABLED
static FATFS fsFlash;
#endif
char *romName;
namespace
{
    constexpr uint32_t CPUFreqKHz = CPU_FREQ_KHZ;

//    constexpr dvi::Config dviConfig_PicoDVI = {
//        .pinTMDS = {10, 12, 14},
//        .pinClock = 8,
//        .invert = true,
//    };
//
//    constexpr dvi::Config dviConfig_PicoDVISock = {
//       .pinTMDS = {12, 18, 16},
//        .pinClock = 14,
//        .invert = false,
//    };

//    std::unique_ptr<dvi::DVI> dvi_;

    static constexpr uintptr_t NES_FILE_ADDR = 0x10080000;

   ROMSelector romSelector_;
   // util::ExclusiveProc exclProc_;

    enum class ScreenMode
    {
        SCANLINE_8_7,
        NOSCANLINE_8_7,
        SCANLINE_1_1,
        NOSCANLINE_1_1,
        MAX,
    };
    ScreenMode screenMode_{};

    bool scaleMode8_7_ = true;

    void applyScreenMode()
    {
        bool scanLine = false;

        switch (screenMode_)
        {
        case ScreenMode::SCANLINE_1_1:
            scaleMode8_7_ = false;
            scanLine = true;
            break;

        case ScreenMode::SCANLINE_8_7:
            scaleMode8_7_ = true;
            scanLine = true;
            break;

        case ScreenMode::NOSCANLINE_1_1:
            scaleMode8_7_ = false;
            scanLine = false;
            break;

        case ScreenMode::NOSCANLINE_8_7:
            scaleMode8_7_ = true;
            scanLine = false;
            break;
        }

        //dvi_->setScanLine(scanLine);
    }
}

// #define CC(x) (((x >> 1) & 15) | (((x >> 6) & 15) << 4) | (((x >> 11) & 15) << 8))
// #define CC(x) (((x>>12)&15))|(((x>>8)&15)<<4)|(((x>>4)&15)<<8)|(((x)&15)<<12)

// #define CC(x) ((((x >> 11) & 31) << 0) | (((x >> 5) & 63) << 5) | (((x >> 0) & 31) << 11))
#define CC(x) (x & 32767)
const WORD __not_in_flash_func(NesPalette)[64] = {
    /*
    CC(0x39ce), CC(0x1071), CC(0x0015), CC(0x2013), CC(0x440e), CC(0x5402), CC(0x5000), CC(0x3c20),
    CC(0x20a0), CC(0x0100), CC(0x0140), CC(0x00e2), CC(0x0ceb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x5ef7), CC(0x01dd), CC(0x10fd), CC(0x401e), CC(0x5c17), CC(0x700b), CC(0x6ca0), CC(0x6521),
    CC(0x45c0), CC(0x0240), CC(0x02a0), CC(0x0247), CC(0x0211), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x1eff), CC(0x2e5f), CC(0x223f), CC(0x79ff), CC(0x7dd6), CC(0x7dcc), CC(0x7e67),
    CC(0x7ae7), CC(0x4342), CC(0x2769), CC(0x2ff3), CC(0x03bb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x579f), CC(0x635f), CC(0x6b3f), CC(0x7f1f), CC(0x7f1b), CC(0x7ef6), CC(0x7f75),
    CC(0x7f94), CC(0x73f4), CC(0x57d7), CC(0x5bf9), CC(0x4ffe), CC(0x0000), CC(0x0000), CC(0x0000),
*/

CC(0xAE73),CC(0xD120),CC(0x1500),CC(0x1340),CC(0x0E88),CC(0x02A8),CC(0x00A0),CC(0x4078),
CC(0x6041),CC(0x2002),CC(0x8002),CC(0xE201),CC(0xEB19),CC(0x0000),CC(0x0000),CC(0x0000),
CC(0xF7BD),CC(0x9D03),CC(0xDD21),CC(0x1E80),CC(0x17B8),CC(0x0BE0),CC(0x40D9),CC(0x61CA),
CC(0x808B),CC(0xA004),CC(0x4005),CC(0x8704),CC(0x1104),CC(0x0000),CC(0x0000),CC(0x0000),
CC(0xFFFF),CC(0xFF3D),CC(0xBF5C),CC(0x5FA4),CC(0xDFF3),CC(0xB6FB),CC(0xACFB),CC(0xC7FC),
CC(0xE7F5),CC(0x8286),CC(0xE94E),CC(0xD35F),CC(0x5B07),CC(0x0000),CC(0x0000),CC(0x0000),
CC(0xFFFF),CC(0x3FAF),CC(0xBFC6),CC(0x5FD6),CC(0x3FFE),CC(0x3BFE),CC(0xF6FD),CC(0xD5FE),
CC(0x34FF),CC(0xF4E7),CC(0x97AF),CC(0xF9B7),CC(0xFE9F),CC(0x0000),CC(0x0000),CC(0x0000),


};    


uint32_t getCurrentNVRAMAddr()
{

    if (!romSelector_.getCurrentROM())
    {
        return {};
    }
    int slot = romSelector_.getCurrentNVRAMSlot();
    if (slot < 0)
    {
        return {};
    }
    printf("SRAM slot %d\n", slot);
    return NES_FILE_ADDR - SRAM_SIZE * (slot + 1);

}


void saveNVRAM()
{
    if (!SRAMwritten)
    {
        printf("SRAM not updated.\n");
        return;
    }

    printf("save SRAM\n");
    // exclProc_.setProcAndWait([]
    //                          {
        static_assert((SRAM_SIZE & (FLASH_SECTOR_SIZE - 1)) == 0);
        if (auto addr = getCurrentNVRAMAddr())
        {
            auto ofs = addr - XIP_BASE;
            printf("write flash %x\n", ofs);
            {
                Frens::flash_lockout_start();
                uint32_t ints = save_and_disable_interrupts();
                flash_range_erase(ofs, SRAM_SIZE);
                flash_range_program(ofs, SRAM, SRAM_SIZE);
                restore_interrupts(ints);
                Frens::flash_lockout_end();
            }
         } //});
    printf("done\n");

    SRAMwritten = false;
}

void loadNVRAM()
{
    if (auto addr = getCurrentNVRAMAddr())
    {
        printf("load SRAM %x\n", addr);
        memcpy(SRAM, reinterpret_cast<void *>(addr), SRAM_SIZE);
    }
    SRAMwritten = false;
}

extern int APU_Mute;

void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
#if 0
    static constexpr int LEFT = 1 << 6;
    static constexpr int RIGHT = 1 << 7;
    static constexpr int UP = 1 << 4;
    static constexpr int DOWN = 1 << 5;
    static constexpr int SELECT = 1 << 2;
    static constexpr int START = 1 << 3;
    static constexpr int A = 1 << 0;
    static constexpr int B = 1 << 1;

    static DWORD prevButtons[2]{};
    static int rapidFireMask[2]{};
    static int rapidFireCounter = 0;

    ++rapidFireCounter;
    bool reset = false;

    for (int i = 0; i < 2; ++i)
    {
        auto &dst = i == 0 ? *pdwPad1 : *pdwPad2;
        auto &gp = io::getCurrentGamePadState(i);

        int v = (gp.buttons & io::GamePadState::Button::LEFT ? LEFT : 0) |
                (gp.buttons & io::GamePadState::Button::RIGHT ? RIGHT : 0) |
                (gp.buttons & io::GamePadState::Button::UP ? UP : 0) |
                (gp.buttons & io::GamePadState::Button::DOWN ? DOWN : 0) |
                (gp.buttons & io::GamePadState::Button::A ? A : 0) |
                (gp.buttons & io::GamePadState::Button::B ? B : 0) |
                (gp.buttons & io::GamePadState::Button::SELECT ? SELECT : 0) |
                (gp.buttons & io::GamePadState::Button::START ? START : 0) |
                0;

        int rv = v;
        if (rapidFireCounter & 2)
        {
            // 15 fire/sec
            rv &= ~rapidFireMask[i];
        }

        dst = rv;

        auto p1 = v;
        auto pushed = v & ~prevButtons[i];
        if (p1 & SELECT)
        {
            if (pushed & LEFT)
            {
                saveNVRAM();
                romSelector_.prev();
                reset = true;
            }
            if (pushed & RIGHT)
            {
                saveNVRAM();
                romSelector_.next();
                reset = true;
            }
            if (pushed & START)
            {
                saveNVRAM();
                reset = true;
            }
            if (pushed & A)
            {
                rapidFireMask[i] ^= io::GamePadState::Button::A;
            }
            if (pushed & B)
            {
                rapidFireMask[i] ^= io::GamePadState::Button::B;
            }
            if (pushed & UP)
            {
                screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) - 1) & 3);
                applyScreenMode();
            }
            else if (pushed & DOWN)
            {
                screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) + 1) & 3);
                applyScreenMode();
            }
        }

        prevButtons[i] = v;
    }

    *pdwSystem = reset ? PAD_SYS_QUIT : 0;
#endif
    static constexpr int _LEFT = 1 << 6;
    static constexpr int _RIGHT = 1 << 7;
    static constexpr int _UP = 1 << 4;
    static constexpr int _DOWN = 1 << 5;
    static constexpr int _SELECT = 1 << 2;
    static constexpr int _START = 1 << 3;
    static constexpr int _AA = 1 << 0;
    static constexpr int _BB = 1 << 1;

    static DWORD prevButtons[2]{};
    static int rapidFireMask[2]{};
    static int rapidFireCounter = 0;

    ++rapidFireCounter;
    bool reset = false;

#ifdef CONTROLLER_NUNCHUCK
    uint8_t nc[8];
    bool nc_ok = nunchuck_read(nc);
    // Buttons are active low in bytes 6 and 7
    uint8_t b6 = nc_ok ? nc[6] : 0xFF;
    uint8_t b7 = nc_ok ? nc[7] : 0xFF;
#endif

    for (int i = 0; i < 2; ++i){

    auto &dst = i == 0 ? *pdwPad1 : *pdwPad2;
    if (i != 0) {
        dst = 0;
        continue;
    }
    int v=0;
#ifdef CONTROLLER_NUNCHUCK
    if (!(b7 & 0x01)) v |= _UP;
    if (!(b6 & 0x40)) v |= _DOWN;
    if (!(b7 & 0x02)) v |= _LEFT;
    if (!(b6 & 0x80)) v |= _RIGHT;
    if (!(b7 & 0x10)) v |= _AA;
    if (!(b7 & 0x40)) v |= _BB;
    if (!(b6 & 0x10)) v |= _SELECT;
    if (!(b6 & 0x04)) v |= _START;
#elif defined(CONTROLLER_GPIO_BUTTONS)
    // Waveshare Pico LCD 1.3" — all active low, pull-ups enabled in key_init()
    if (!gpio_get(JOY_UP))    v |= _UP;
    if (!gpio_get(JOY_DOWN))  v |= _DOWN;
    if (!gpio_get(JOY_LEFT))  v |= _LEFT;
    if (!gpio_get(JOY_RIGHT)) v |= _RIGHT;
    if (!gpio_get(BTN_A))     v |= _AA;
    if (!gpio_get(BTN_B))     v |= _BB;
    if (!gpio_get(BTN_X))     v |= _SELECT;
    if (!gpio_get(BTN_Y))     v |= _START;
    // Joystick center (JOY_CTR) resets the emulator when held with Start
#endif

    int rv = v;
        if (rapidFireCounter % 8 == 0)
        {
            // 
            rv &= ~rapidFireMask[i];
        }

        dst = rv;
        auto p1 = v;
        auto pushed = v & ~prevButtons[i];
        if (p1 & _SELECT)
        {
            if (pushed & _LEFT)
            {
                /* SELECT + LEFT: previous ROM in TAR archive (no-op for single-ROM). */
                saveNVRAM();
                romSelector_.prev();
                reset = true;
            }
            if (pushed & _RIGHT)
            {
                /* SELECT + RIGHT: next ROM in TAR archive (no-op for single-ROM). */
                saveNVRAM();
                romSelector_.next();
                reset = true;
            }
            if (pushed & _START)
            {
                saveNVRAM();
                reset = true;
            }
            if (pushed & _AA)
            {
                rapidFireMask[i] ^= _AA;
                rapidFireCounter = 0;
            }
            if (pushed & _BB)
            {
                rapidFireMask[i] ^= _BB;
                rapidFireCounter = 0;
            }
            if (pushed & _UP)
            {
                APU_Mute = 0;

                // screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) - 1) & 3);
                // applyScreenMode();
            }
            else if (pushed & _DOWN)
            {
                APU_Mute = 1;

                // screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) + 1) & 3);
                // applyScreenMode();
            }
        }

        prevButtons[i] = *pdwPad1;
    }
    *pdwSystem = reset ? PAD_SYS_QUIT : 0;
}

void InfoNES_MessageBox(const char *pszMsg, ...)
{
    printf("[MSG]");
    va_list args;
    va_start(args, pszMsg);
    vprintf(pszMsg, args);
    va_end(args);
    printf("\n");
}

bool parseROM(const uint8_t *nesFile)
{

    memcpy(&NesHeader, nesFile, sizeof(NesHeader));
    if (!checkNESMagic(NesHeader.byID))
    {
        return false;
    }

    nesFile += sizeof(NesHeader);

    memset(SRAM, 0, SRAM_SIZE);

    if (NesHeader.byInfo1 & 4)
    {
        memcpy(&SRAM[0x1000], nesFile, 512);
        nesFile += 512;
    }

    auto romSize = NesHeader.byRomSize * 0x4000;
    ROM = (BYTE *)nesFile;
    nesFile += romSize;

    if (NesHeader.byVRomSize > 0)
    {
        auto vromSize = NesHeader.byVRomSize * 0x2000;
        VROM = (BYTE *)nesFile;
        nesFile += vromSize;
    }

    return true;
}

void InfoNES_ReleaseRom()
{
    ROM = nullptr;
    VROM = nullptr;
}

void InfoNES_SoundInit()
{
}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
    return 0;
}

void InfoNES_SoundClose()
{
}

int __not_in_flash_func(InfoNES_GetSoundBufferSize)()
{
   return audioRing.writable_size();
}

#define TARGET_LATENCY_BYTES 1500 // Approx 2 frames of audio at 22050Hz

/*
 *  call from InfoNES_pAPUHsync
 */
void __not_in_flash_func(InfoNES_SoundOutput)(int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3, BYTE *wave4, BYTE *wave5)
{
#ifdef DISABLE_AUDIO
    /* Audio disabled at compile time — drop samples on the floor. Skipping the
     * audioRing write also skips the latency-throttle loop below, which would
     * otherwise hang waiting for core1 to drain. */
    return;
#endif
    static int test_i=0;

    SoundOutputBuilding = true;
    
    // Use snd_buf as a scratchpad buffer
    // Ensure we don't overflow snd_buf if samples is unexpectedly large (though usually ~735)
    int remaining = samples;
    while (remaining > 0)
    {
        // Latency Control / Synchronization
        // If the buffer is too full, wait for Core 1 to consume some samples.
        // This throttles Core 0 to match the audio playback speed.
        while (audioRing.readable_size() > TARGET_LATENCY_BYTES)
        {
            sleep_us(100); 
        }

        int n = remaining;
        if (n > AUDIO_BUF_SIZE) n = AUDIO_BUF_SIZE;

        auto p = snd_buf;
        int ct = n;
        while (ct--)
        {
            uint8_t w1 = *wave1++;
            uint8_t w2 = *wave2++;
            uint8_t w3 = *wave3++; // triangle
            uint8_t w4 = *wave4++; // noise
            uint8_t w5 = *wave5++; // DPCM

#if defined(I2S_AUDIO)
             /* The five channels do NOT share a range: the two pulses and the
              * triangle are 0..255 (pulse tables hold 0x11 * vol, vol 0..15),
              * noise is only 0..15 (ApuC4Vol) and DPCM 0..63. Normalise each to
              * 0..255 before averaging, then saturate — the /4 formula below
              * reaches 332 on a loud frame and wraps around inside the BYTE,
              * which is audible as crackle on peaks. */
             {
                 int mix = (w1 + w2 + w3 + w4 * 17 + w5 * 4) / 5;

                 /* The APU's signal is UNIPOLAR: silence is 0, not 128, and
                  * the DC level rides up and down with how many channels are
                  * sounding. Track that DC with a one-pole filter (shift 7 =
                  * ~27 Hz corner at 22050) and subtract it, so what reaches the
                  * DAC is centred on 128 and the gain below is applied to the
                  * audio rather than to the offset. */
                 static int32_t dc_acc = 0;    /* mix level, 8.8 fixed point */
                 dc_acc += (((int32_t)mix << 8) - dc_acc) >> 7;
                 int ac = mix - (dc_acc >> 8);

                 int v = 128 + (ac * I2S_GAIN_PERCENT) / 100;
                 if (v < 0) v = 0; else if (v > 255) v = 255;
                 *p++ = (uint8_t)v;
             }
#elif defined(ILI9341)
             *p++ =  (((w1 * 2 + w2 * 2)/2)  + w3 * 1  + w4 * 1 * 4 + w5 * 2 * 4) / 4;
#elif defined(ST7789)
             *p++ =  (((w1 * 2 + w2 * 2)/2)  + w3 * 1  + w4 * 1 * 4 + w5 * 2 * 4) * 16;
#endif
        }

        // Write to RingBuffer, wait if full or just spin? 
        // For now, valid write only what fits or overwrite?
        // simple write
        audioRing.write(snd_buf, n);
        
        remaining -= n;
    }

    SoundOutputBuilding = false;
}



extern WORD PC;


////
/*
 *
 */
static void display_write_command(const uint8_t command)
{
    /* Set DC low to denote incoming command. */
    gpio_put(DISPLAY_PIN_DC, 0);

    /* Set CS low to reserve the SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 0);

    spi_write_blocking(DISPLAY_SPI_PORT, &command, 1);

    /* Set CS high to ignore any traffic on SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 1);
}

static void display_write_data(const uint8_t *data, size_t length)
{
    size_t sent = 0;

    if (0 == length) {
        return;
    };

    /* Set DC high to denote incoming data. */
    gpio_put(DISPLAY_PIN_DC, 1);

    /* Set CS low to reserve the SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 0);

    spi_write_blocking(DISPLAY_SPI_PORT, data, length);

    /* Set CS high to ignore any traffic on SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 1);
}
 void __not_in_flash_func(display_set_address)(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    uint8_t command;
    uint8_t data[4];
    static uint16_t prev_x1, prev_x2, prev_y1, prev_y2;

    x1 = x1 + DISPLAY_OFFSET_X;
    y1 = y1 + DISPLAY_OFFSET_Y;
    x2 = x2 + DISPLAY_OFFSET_X;
    y2 = y2 + DISPLAY_OFFSET_Y;

    /* Change column address only if it has changed. */
    if ((prev_x1 != x1 || prev_x2 != x2)) {
        display_write_command(DCS_SET_COLUMN_ADDRESS);
        data[0] = x1 >> 8;
        data[1] = x1 & 0xff;
        data[2] = x2 >> 8;
        data[3] = x2 & 0xff;
        display_write_data(data, 4);

        prev_x1 = x1;
        prev_x2 = x2;
    }

    /* Change page address only if it has changed. */
    if ((prev_y1 != y1 || prev_y2 != y2)) {
        display_write_command(DCS_SET_PAGE_ADDRESS);
        data[0] = y1 >> 8;
        data[1] = y1 & 0xff;
        data[2] = y2 >> 8;
        data[3] = y2 & 0xff;
        display_write_data(data, 4);

        prev_y1 = y1;
        prev_y2 = y2;
    }
    // 
    display_write_command(DCS_WRITE_MEMORY_START);
}
////
/*
 *  setting column and page, start and stop
 */
void ili9341_infones_frame_timing_register_init()
{
    display_set_address(0, NES_FIRST_SCANLINE, DISPLAY_WIDTH - 1, NES_LAST_SCANLINE);
    /* CS stays HIGH — will be driven low at the start of the first rendered scanline */
}
void st7789_infones_frame_timing_register_init()
{
    display_set_address(0, NES_FIRST_SCANLINE, DISPLAY_WIDTH - 1, NES_LAST_SCANLINE);
    /* CS stays HIGH — will be driven low at the start of the first rendered scanline */
}


static void __not_in_flash_func(blink_led)(void)
{
    gpio_xor_mask(1<<LED_PIN);
}
/* Cleared by speed_control() when the previous frame missed its deadline:
 * the next frame is emulated in full but never sent to the panel. Read by
 * InfoNES_PostDrawLine(). */
volatile bool draw_this_frame = true;

static void __not_in_flash_func(speed_control)(void)
{
  static uint64_t deadline = 0;

// frame timing control
  uint64_t cur_time = time_us_64();
  // 1/60 = 16666 us
  if (deadline == 0) deadline = cur_time;

  if ((int64_t)(cur_time - deadline) <= 0)
  {
      /* Made the deadline with time to spare — wait it out and draw the next
       * frame to the panel as usual. */
      while ((int64_t)(time_us_64() - deadline) < 0) tight_loop_contents();
      draw_this_frame = true;
  }
  else
  {
      /* Behind. A drawn frame costs ~14.8 ms of SPI on a 320-wide panel at
       * 80 MHz, which is most of the 16.6 ms budget, so skipping the transfer
       * is what buys the time back. Emulation still runs every frame — and
       * that matters for more than smoothness: the APU generates a fixed 367
       * samples per emulated frame, so emulating at 45 fps means 16500
       * samples/s against the 22050/s the DAC consumes, and the soundtrack
       * plays a quarter too slow with the shortfall padded. */
      draw_this_frame = false;
      /* A long stall (ROM load, menu, flash write) must not leave us trying to
       * catch up for thousands of frames. */
      if ((int64_t)(cur_time - deadline) > 100000) deadline = cur_time;
  }
  deadline += 16666;

  // blink_led();

  // uint64_t cur_time = time_us_64();
  // if (last_blink + 16666 < cur_time) {
  //   gpio_xor_mask(1<<LED_PIN);
  //   last_blink = cur_time;
  //   if(frame_column_step==0 && FRAME_COLUMN_WIDTH>0) FRAME_COLUMN_WIDTH--;
  // }else{
  //   if(frame_column_step==0 && FRAME_COLUMN_WIDTH<256) FRAME_COLUMN_WIDTH++;
  // }
}

#if 1
static BYTE old_frame_skip_counter;
#ifdef I2S_AUDIO
#define AUDIO_CORE_START() audio_core_start()
#define AUDIO_CORE_STOP()  audio_core_stop()
#else
#define AUDIO_CORE_START() ((void)0)
#define AUDIO_CORE_STOP()  ((void)0)
#endif

/* Set once core1 can be parked by multicore_lockout_start_blocking(). Core0
 * must not erase or program flash while core1 is running code from XIP — the
 * bus is unusable during the operation and core1 faults or hangs. The PWM
 * consumer happened to be entirely RAM-resident and got away with it; the I2S
 * one calls into the SDK (sleep_us, printf), so it does not. */
static volatile bool core1_lockout_ready = false;

/* True while core1 is parked, so flash_lockout_end() cannot try to release a
 * lockout that never took. */
static bool core1_locked_out = false;

namespace Frens
{
    void flash_lockout_start()
    {
        core1_locked_out = false;
        if (!core1_lockout_ready) return;
        /* Bounded, not blocking: if core1 cannot be parked we want to know
         * about it on the console instead of hanging here forever. */
        /* Short timeout on purpose: this is belt-and-braces now that core1's
         * loop is entirely RAM-resident, and a ROM copy is ~10 flash blocks —
         * a long timeout per block would add seconds to every load. */
        core1_locked_out = multicore_lockout_start_timeout_us(5000);
        if (!core1_locked_out) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                printf("[flash] core1 lockout timed out — relying on core1 being RAM-resident\n");
            }
        }
    }
    void flash_lockout_end()
    {
        if (!core1_locked_out) return;
        if (!multicore_lockout_end_timeout_us(5000)) printf("[flash] core1 release TIMED OUT\n");
        core1_locked_out = false;
    }
}

#ifdef I2S_AUDIO
extern uint32_t FrameCounter;   /* defined below; core0 bumps it per frame */
void core1_main();

/* core1 (the audio consumer) runs only while a game is running.
 *
 * The menu has no sound to play, and it is the menu that copies the selected
 * ROM into flash — with core1 stopped there is simply no second core to fall
 * over when XIP goes away, instead of a lockout handshake to get right. Saves
 * also write flash mid-game, so core1 stays RAM-resident regardless. */
static bool audio_core_running = false;

static void audio_core_start()
{
    if (audio_core_running) return;
    audioRing.reset();            /* drop whatever the last game left behind */
    multicore_launch_core1(core1_main);
    audio_core_running = true;
}

static void audio_core_stop()
{
    if (!audio_core_running) return;
    multicore_reset_core1();
    /* Only now: while core1 was alive the two DMA channels were chained to each
     * other, and they would happily keep replaying the last two buffers. */
    i2s_output_stop();
    audio_core_running = false;
    core1_lockout_ready = false;
}
/* Source for the I2S ping-pong buffers: the same ring the PWM path drains. */
static int __not_in_flash_func(i2s_pull)(uint8_t *dst, int count)
{
    return audioRing.read(dst, count);
}
#endif

void __not_in_flash_func(core1_main)()
{
    multicore_lockout_victim_init();
    core1_lockout_ready = true;
#ifdef I2S_AUDIO
    /* External I2S DAC (PIO + DMA). i2s_output_pump() refills whichever
     * ping-pong buffer just drained; a shortfall is padded inside.
     *
     * Do NOT spin on the pump: a buffer lasts ~11.6 ms, so polling every 500 us
     * is 20x more often than needed, while a tight loop hammers the DMA
     * registers over the bus and steals bandwidth from core0's emulation and
     * from the display DMA. */
    i2s_output_init(22050, i2s_pull);

    /* Nothing in this loop may call into flash: core0 erases and programs
     * flash (ROM copy, SRAM saves) with XIP down, and the multicore lockout
     * that is supposed to park core1 for that is not completing on this board.
     * So the delay polls the timer register directly instead of sleep_us(),
     * i2s_pull/audioRing::read are __not_in_flash_func, and the once-a-second
     * I2S_DEBUG report is printed by core0 in InfoNES_LoadFrame() rather than
     * here — printf is the one thing that would otherwise reach flash. */
    while (true) {
        i2s_output_pump();
        uint32_t t0 = timer_hw->timerawl;
        while (timer_hw->timerawl - t0 < 500) tight_loop_contents();
    }
#else
    audio_init(AUDIO_PIN, 22050);

    while (true) {
        uint8_t *buf = audio_get_buffer();
        if (!buf) continue;

        int n = audioRing.read(buf, AUDIO_BUFFER_SIZE);
        if (n < AUDIO_BUFFER_SIZE) {
            memset(buf + n, 128, AUDIO_BUFFER_SIZE - n);
        }
    }
#endif
}

#endif




uint32_t FrameCounter=0;
uint16_t test_color_bar = 0;
/*
 *  call from InfoNES_HSync() 
 *  in every frame
 */
int __not_in_flash_func(InfoNES_LoadFrame)()
{
#if 0
    gpio_put(LED_PIN, hw_divider_s32_quotient_inlined(dvi_->getFrameCounter(), 60) & 1);
    //    printf("%04x\n", PC);

    tuh_task();
#endif
/*
 *. blink the led and control speed
 */
    speed_control();

/*
 *
 */
    // frame_skip = true;
    if(frame_skip_counter++ == 2){
        frame_skip_counter = 0;
        // frame_skip = false;
        // if(frame_skip == false) frame_skip = true;
        // else frame_skip = false;
            
        /*
         *   sound process : 735 samples per frame
         */  
    }
    // (AUDIO_BUF_SIZE-buf_residue_size)

    /*
    *
    *. AUDIO (not parallel process)
    *
    */
    #if 0
    if(frame_skip_counter == 0){
        int j=0;
        // for(int i=0; i<(AUDIO_BUF_SIZE-buf_residue_size); i+=1,j++){
        //     snd_buf[j]=snd_buf[i];
        // }
        // // snd_buf[j++]=snd_buf[(AUDIO_BUF_SIZE-buf_residue_size)];
        // audio_play_once(snd_buf,j-1);
        int id = audio_play_once(snd_buf,AUDIO_BUF_SIZE-buf_residue_size);        
        // if (id >= 0) audio_source_set_volume(id, 1024);
        buf_residue_size = AUDIO_BUF_SIZE;
    }

        audio_mixer_step();
    #endif
        
/*
 *
 */
#if 0    
    frame_column_step += FRAME_COLUMN_WIDTH;
    test_color_bar = NesPalette[frame_column_step&63];
    if(frame_column_step > 256) frame_column_step = 0;
#endif

    /*
     *   setting frame display column
     *
     *   only column change, page remains the same.
     *
     */

    //     for(int x=0;x<256;x+=1){

    //         hagl_put_pixel(display,x+((320-256)/2),screen_y,scanline_buf_internal[x]);
    //     }
    // return;

#if 0
        uint8_t command;
        uint8_t data[4];
        int x=0;

//// DCS_SET_COLUMN_ADDRESS
                gpio_put(DISPLAY_PIN_DC, 0);

                /* Set CS low to reserve the SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 0);

                command = DCS_SET_COLUMN_ADDRESS;
                spi_write_blocking(DISPLAY_SPI_PORT, &command, 1);

                /* Set CS high to ignore any traffic on SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 1);
////
                /* Set DC high to denote incoming data. */
                gpio_put(DISPLAY_PIN_DC, 1);

                /* Set CS low to reserve the SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 0);

                int x_width_end = (frame_column_step+FRAME_COLUMN_WIDTH > 256)?256:frame_column_step+FRAME_COLUMN_WIDTH;
                data[0] = x+((320-256)/2)+frame_column_step >> 8;
                data[1] = x+((320-256)/2)+frame_column_step & 0xff;
                data[2] = (x+((320-256)/2)+x_width_end-1) >> 8;
                data[3] = (x+((320-256)/2)+x_width_end-1) & 0xff;
                spi_write_blocking(DISPLAY_SPI_PORT, data, 4);

                /* Set CS high to ignore any traffic on SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 1);


//// DCS_WRITE_MEMORY_START
                gpio_put(DISPLAY_PIN_DC, 0);

                /* Set CS low to reserve the SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 0);

                command = DCS_WRITE_MEMORY_START;
                spi_write_blocking(DISPLAY_SPI_PORT, &command, 1);

                /* Set CS high to ignore any traffic on SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 1);


                gpio_put(DISPLAY_PIN_DC, 1);
                gpio_put(DISPLAY_PIN_CS, 0);

#endif



#ifdef I2S_DEBUG
    /* Once a second: is the consumer too fast, or the producer too slow?
     *   buf/s  — I2S buffers played; nominal is 22050/256 = 86
     *   short  — samples padded because the audio ring ran dry
     *   fps    — emulated frames; 60 means the APU made its 22050/s
     * Printed here, on core0, because core1 must stay clear of flash. */
    {
        static uint64_t next_report = 0;
        static uint32_t last_frames = 0;
        uint64_t now = time_us_64();
        if (now >= next_report) {
            if (next_report != 0) {
                uint32_t bufs, shortfall;
                i2s_output_get_stats(&bufs, &shortfall);
                printf("i2s: %lu buf/s (%lu samples/s), %lu short, emu %lu fps\n",
                       (unsigned long)bufs, (unsigned long)bufs * 256,
                       (unsigned long)shortfall,
                       (unsigned long)(FrameCounter - last_frames));
            }
            next_report = now + 1000000;
            last_frames = FrameCounter;
        }
    }
#endif
    return FrameCounter++;
}
#if 0
namespace
{
    dvi::DVI::LineBuffer *currentLineBuffer_{};
}

void __not_in_flash_func(drawWorkMeterUnit)(int timing,
                                            [[maybe_unused]] int span,
                                            uint32_t tag)
{
    if (timing >= 0 && timing < 640)
    {
        auto p = currentLineBuffer_->data();
        p[timing] = tag; // tag = color
    }
}

void __not_in_flash_func(drawWorkMeter)(int line)
{
    if (!currentLineBuffer_)
    {
        return;
    }

    memset(currentLineBuffer_->data(), 0, 64);
    memset(&currentLineBuffer_->data()[320 - 32], 0, 64);
    (*currentLineBuffer_)[160] = 0;
    if (line == 4)
    {
        for (int i = 1; i < 10; ++i)
        {
            (*currentLineBuffer_)[16 * i] = 31;
        }
    }

    constexpr uint32_t clocksPerLine = 800 * 10;
    constexpr uint32_t meterScale = 160 * 65536 / (clocksPerLine * 2);
    util::WorkMeterEnum(meterScale, 1, drawWorkMeterUnit);
    //    util::WorkMeterEnum(160, clocksPerLine * 2, drawWorkMeterUnit);
}
#endif

void __not_in_flash_func(RomSelect_PreDrawLine)(int line)
{
    if(line % 2 == 0){
        RomSelect_SetLineBuffer(scanline_buf_internal_1, 256);
    }else{
        RomSelect_SetLineBuffer(scanline_buf_internal_2, 256);
    }
}

/*
 *  InfoNES_PreDrawLine and 
 *  InfoNES_PostDrawLine
 *  
 *   call from InfoNES_HSync() 
 *  on every scanline 
 */
void __not_in_flash_func(InfoNES_PreDrawLine)(int line)
{
#if 0
    util::WorkMeterMark(0xaaaa);
    auto b = dvi_->getLineBuffer();
    util::WorkMeterMark(0x5555);
    InfoNES_SetLineBuffer(b->data() + 32, b->size());
    //    (*b)[319] = line + dvi_->getFrameCounter();

    currentLineBuffer_ = b;
#endif
    if(line % 2 == 0){
        InfoNES_SetLineBuffer(scanline_buf_internal_1, 256);
    }else{
        InfoNES_SetLineBuffer(scanline_buf_internal_2, 256);
    }
}

void __not_in_flash_func(InfoNES_PostDrawLine)(int line)
{
    /* Frame dropped by speed_control(): emulate it, but send nothing. The
     * in-flight DMA from the last drawn frame is waited for below, on the
     * first line of the next drawn one. */
    if (!draw_this_frame) return;

#if 0
#if !defined(NDEBUG)
    util::WorkMeterMark(0xffff);
    drawWorkMeter(line);
#endif

    assert(currentLineBuffer_);
    dvi_->setLineBuffer(line, currentLineBuffer_);
    currentLineBuffer_ = nullptr;
#endif
//     #define screen_x_step 4
// if(line == 4){
//     screen_x_start+=screen_x_step;
//     if(screen_x_start>320) screen_x_start=0;
//     screen_x=screen_x_start;
// }
// for(int i=0;i<screen_x_step;i++){
//             hagl_put_pixel(display,line,screen_x+((320-256)/2),scanline_buf_internal[screen_x]);
        
//             screen_x++;
//             if(screen_x>320){ 
//                 screen_x=0;
//             }
//          }

/*
 *  frame skip
 */
// if(frame_skip) return;


        // screen_y = line;

    // if(line_drawing==false){

        // if(line_drawing == false){
        // screen_y = line;
        // __builtin_memcpy(scanline_buf_outgoing,scanline_buf_internal,sizeof(WORD)*256);
        // sleep_us(100);
        // line_drawing = true;
        // }

        // __builtin_memcpy(framebuffer+(sizeof(BYTE)*line),scanline_buf_internal,sizeof(BYTE)*256);
    // if(line == 4){ 
    //     frame_skip--;
    //     if(frame_skip<0) frame_skip=0;
    // }

        /*
         *  each scanline only display partial column 
         *  spi_write is here, also contorl the speed of frame rate
         *  less the FRAME_COLUMN_WIDTH, speed up  the frame rate
         */ 

        // uint8_t command;
        // uint8_t data[4];

// Continue Write
            // display_write_data(&scanline_buf_internal[x], 2);
                /* Set DC high to denote incoming data. */
                // gpio_put(DISPLAY_PIN_DC, 1);

                /* Set CS low to reserve the SPI bus. */
                // gpio_put(DISPLAY_PIN_CS, 0);
#if 0
            for(int x=frame_column_step;x<frame_column_step+FRAME_COLUMN_WIDTH && x<256;x+=1){
                // data[1] = scanline_buf_internal[x] >> 8;
                // data[0] = scanline_buf_internal[x] & 0xff;

                spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&scanline_buf_internal[x], 2);

                // spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&test_color_bar, 2);
            }
#endif
#if 0
                 spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&scanline_buf_internal[frame_column_step], FRAME_COLUMN_WIDTH*2);
                 // spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&test_color_bar, FRAME_COLUMN_WIDTH*2);
#endif
#if 0
                 spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)scanline_buf_internal, 256*2);
#endif
    WORD *fb;
    if(line % 2 == 0){
        fb = scanline_buf_internal_1;
    }else{
        fb = scanline_buf_internal_2;
    }        
    dma_channel_wait_for_finish_blocking(display_dma_channel);
    if (line == NES_FIRST_SCANLINE) {
        /* First rendered scanline: drain SPI TX FIFO then drive CS low for the frame.
         * On SHARED_SPI_BUS targets also re-issue display_set_address to correct any
         * write-pointer corruption caused by SD card traffic on the shared bus. */
        while (spi_is_busy(DISPLAY_SPI_PORT)) tight_loop_contents();
#ifdef SHARED_SPI_BUS
        display_set_address(0, NES_FIRST_SCANLINE, DISPLAY_WIDTH - 1, NES_LAST_SCANLINE);
#endif
        gpio_put(DISPLAY_PIN_DC, 1);
        gpio_put(DISPLAY_PIN_CS, 0);
    }
#if DISPLAY_WIDTH == 320
    /* Scale NES 256px wide → 320px wide (nearest-neighbour, right-to-left in-place). */
    for (int i = 319; i >= 0; i--) fb[i] = fb[i * 256 / 320];
#else
    /* Crop NES 256px wide → 240px wide: drop 8px overscan on each side. */
    for (int i = 0; i < 240; i++) fb[i] = fb[i + 8];
#endif
    dma_channel_set_trans_count(display_dma_channel, DISPLAY_WIDTH * 2, false);
    dma_channel_set_read_addr(display_dma_channel, fb, true);
                /* Set CS high to ignore any traffic on SPI bus. */
                // gpio_put(DISPLAY_PIN_CS, 1);



    // } // for
}

bool loadAndReset()
{

    auto rom = romSelector_.getCurrentROM();
    if (!rom)
    {
        printf("ROM does not exists.\n");
        return false;
    }

    if (!parseROM(rom))
    {
        printf("NES file parse error.\n");
        return false;
    }
    loadNVRAM();

    if (InfoNES_Reset() < 0)
    {
        printf("NES reset error.\n");
        return false;
    }

    return true;

}

int InfoNES_Menu()
{
    // InfoNES_Main() のループで最初に呼ばれる
    loadAndReset();
    return 0;
}



#ifdef CONTROLLER_NUNCHUCK
static void nunchuck_init() {
    i2c_init(NUNCHUCK_I2C_BUS, 400 * 1000);
    gpio_set_function(NUNCHUCK_SDA, GPIO_FUNC_I2C);
    gpio_set_function(NUNCHUCK_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(NUNCHUCK_SDA);
    gpio_pull_up(NUNCHUCK_SCL);
    uint8_t init1[] = {0xF0, 0x55};
    i2c_write_blocking(NUNCHUCK_I2C_BUS, NUNCHUCK_ADDR, init1, 2, false);
    sleep_ms(1);
    uint8_t init2[] = {0xFB, 0x00};
    i2c_write_blocking(NUNCHUCK_I2C_BUS, NUNCHUCK_ADDR, init2, 2, false);
    sleep_ms(1);
    uint8_t init3[] = {0xFE, 0x03};
    i2c_write_blocking(NUNCHUCK_I2C_BUS, NUNCHUCK_ADDR, init3, 2, false);
    sleep_ms(1);
}

static bool nunchuck_read(uint8_t data[8]) {
    uint8_t reg = 0x00;
    if (i2c_write_blocking(NUNCHUCK_I2C_BUS, NUNCHUCK_ADDR, &reg, 1, false) < 0)
        return false;
    sleep_us(200);
    return i2c_read_blocking(NUNCHUCK_I2C_BUS, NUNCHUCK_ADDR, data, 8, false) == 8;
}
#endif /* CONTROLLER_NUNCHUCK */

static void key_init() {
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
#ifdef CONTROLLER_NUNCHUCK
    nunchuck_init();
#elif defined(CONTROLLER_GPIO_BUTTONS)
    /* Waveshare Pico LCD 1.3" — all buttons/joystick active low, enable internal pull-ups. */
    const uint btn_pins[] = {BTN_A, BTN_B, BTN_X, BTN_Y,
                              JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT, JOY_CTR};
    for (uint pin : btn_pins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }
#endif
}








static void display_spi_master_init()
{
    // https://github.com/Bodmer/TFT_eSPI/discussions/2432
// Get the processor sys_clk frequency in Hz
 uint32_t freq = clock_get_hz(clk_sys);

 // clk_peri does not have a divider, so input and output frequencies will be the same
 clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    freq,
                    freq);


    gpio_set_function(DISPLAY_PIN_DC, GPIO_FUNC_SIO);
    gpio_set_dir(DISPLAY_PIN_DC, GPIO_OUT);

    gpio_set_function(DISPLAY_PIN_CS, GPIO_FUNC_SIO);
    gpio_set_dir(DISPLAY_PIN_CS, GPIO_OUT);

    gpio_set_function(DISPLAY_PIN_CLK,  GPIO_FUNC_SPI);
    gpio_set_function(DISPLAY_PIN_MOSI, GPIO_FUNC_SPI);

    if (DISPLAY_PIN_MISO > 0) {
        gpio_set_function(DISPLAY_PIN_MISO, GPIO_FUNC_SPI);
    }

    /* Set CS high to ignore any traffic on SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 1);

    spi_init(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);

    uint32_t baud = spi_set_baudrate(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);
    uint32_t peri = clock_get_hz(clk_peri);
    uint32_t sys = clock_get_hz(clk_sys);

// DMA init
    display_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config channel_config = dma_channel_get_default_config(display_dma_channel);
    channel_config_set_transfer_data_size(&channel_config, DMA_SIZE_8);
    if (spi0 == DISPLAY_SPI_PORT) {
        channel_config_set_dreq(&channel_config, DREQ_SPI0_TX);
    } else {
        channel_config_set_dreq(&channel_config, DREQ_SPI1_TX);
    }
    dma_channel_set_config(display_dma_channel, &channel_config, false);
    dma_channel_set_write_addr(display_dma_channel, &spi_get_hw(DISPLAY_SPI_PORT)->dr, false);
}

void display_init()
{

    /* Init the spi driver. */
    display_spi_master_init();
    sleep_ms(100);

    /* Reset the display. */
    if (DISPLAY_PIN_RST > 0) {
        gpio_set_function(DISPLAY_PIN_RST, GPIO_FUNC_SIO);
        gpio_set_dir(DISPLAY_PIN_RST, GPIO_OUT);

        gpio_put(DISPLAY_PIN_RST, 0);
        sleep_ms(100);
        gpio_put(DISPLAY_PIN_RST, 1);
        sleep_ms(100);
    }

    /* Send minimal init commands. */
    display_write_command(DCS_SOFT_RESET);
    sleep_ms(200);

    display_write_command(DCS_SET_ADDRESS_MODE);
    uint8_t mode1 = DISPLAY_ADDRESS_MODE;
    display_write_data(&mode1, 1);

    display_write_command(DCS_SET_PIXEL_FORMAT);
    uint8_t mode2 = DISPLAY_PIXEL_FORMAT;
    display_write_data(&mode2, 1);

#ifdef DISPLAY_INVERT
    display_write_command(DCS_ENTER_INVERT_MODE);

#else
    display_write_command(DCS_EXIT_INVERT_MODE);
#endif

    display_write_command(DCS_EXIT_SLEEP_MODE);
    sleep_ms(200);

    display_write_command(DCS_SET_DISPLAY_ON);
    sleep_ms(200);
// // ENDIAN
//     display_write_command(0xf6);
//     uint8_t data1[2]= {0x00,0x01};
//     display_write_data(data1,2);
//     uint8_t data2[2]= {0x00,0x00};
//     display_write_data(data2,2);
//     uint8_t data3[2]= {0x00,0x20};
//     display_write_data(data3,2); // 0x0020 = LSB first

    /* Enable backlight */
    if (DISPLAY_PIN_BL > 0) {
        gpio_set_function(DISPLAY_PIN_BL, GPIO_FUNC_SIO);
        gpio_set_dir(DISPLAY_PIN_BL, GPIO_OUT);

        gpio_put(DISPLAY_PIN_BL, 1);
    }

    /* Set the default viewport to full screen. */
    display_set_address(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);


}
void display_clear()
{
    display_set_address(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    BYTE pixel[2] = {0x00, 0x00};
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
        display_write_data(pixel, 2);
    }
}

bool initSDCard()
{
    FRESULT fr;
    TCHAR str[40];
    sleep_ms(1000);

    printf("Mounting SDcard");
    fr = FR_NOT_READY;
    for (int attempt = 0; attempt < 3 && fr != FR_OK; attempt++) {
        if (attempt > 0) {
            printf(" retry %d", attempt);
            sleep_ms(500);
        }
        fr = f_mount(&fs, "0:", 1);
    }
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "SD card mount error: %d", fr);
        printf("%s\n", ErrorMessage);
        spi_set_baudrate(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);
        return false;
    }
    printf("\n");

    fr = f_chdir("/");
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot change dir to / : %d", fr);
        printf("%s\n", ErrorMessage);
        spi_set_baudrate(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);
        return false;
    }
    // for f_getcwd to work, set
    //   #define FF_FS_RPATH        2
    // in drivers/fatfs/ffconf.h
    fr = f_getcwd(str, sizeof(str));
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot get current dir: %d", fr);
        printf("%s\n", ErrorMessage);
        spi_set_baudrate(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);
        return false;
    }
    printf("Current directory: %s\n", str);
    printf("Creating directory %s\n", GAMESAVEDIR);
    fr = f_mkdir(GAMESAVEDIR);
    if (fr != FR_OK)
    {
        if (fr == FR_EXIST)
        {
            printf("Directory already exists.\n");
        }
        else
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot create dir %s: %d", GAMESAVEDIR, fr);
            printf("%s\n", ErrorMessage);
            spi_set_baudrate(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);
            return false;
        }
    }
    // SD card init left SPI at CLK_FAST (30 MHz). Restore the display's
    // configured speed so display performance is not affected.
    spi_set_baudrate(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);
    return true;
}

#ifdef FLASHFS_ENABLED
/* Mount the read-only FAT32 image in XIP flash (drivers/flashfs). Called only
 * when initSDCard() returned false. f_chdrive("1:") switches the default drive
 * so menu/romlister paths target the flash image without any other code change. */
bool initFlashFS()
{
    printf("No SD card — trying flash FAT image at 0x%08x ... ", (unsigned)FLASHFS_BASE_ADDR);
    /* Preflight: does the FAT boot-sector signature exist at offset 510?
     * If not, the image was never flashed (or was wiped) — distinguish that
     * common case from "image is present but corrupt" with a clearer message. */
    if (!flashfs_image_present()) {
        printf("no image flashed (use tools/mkromfs.sh + picotool load -o 0x%08x)\n",
               (unsigned)FLASHFS_BASE_ADDR);
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "No FAT image in flash");
        return false;
    }
    FRESULT fr = f_mount(&fsFlash, "1:", 1);
    if (fr != FR_OK) {
        printf("mount error %d (FatFs FRESULT)\n", fr);
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Flash FS mount error: %d", fr);
        return false;
    }
    fr = f_chdrive("1:");
    if (fr != FR_OK) {
        printf("chdrive error %d\n", fr);
        return false;
    }
    fr = f_chdir("/");
    if (fr != FR_OK) {
        printf("chdir / error %d\n", fr);
        return false;
    }
    printf("ok\n");
    return true;
}
#endif

int main()
{
    char selectedRom[80];
    romName = selectedRom;
    char errMSG[ERRORMESSAGESIZE];
    errMSG[0] = selectedRom[0] = 0;
    ErrorMessage = errMSG;

#ifdef OVERCLOCK_VREG
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(100);
#endif
    set_sys_clock_khz(CPUFreqKHz, true);


    stdio_init_all();
    key_init();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

#ifdef SHARED_SPI_BUS
    /* On SHARED_SPI_BUS targets, SD card and touch controller share spi1 with the LCD.
     * Drive their CS pins HIGH before display_init() so they ignore the LCD init traffic
     * and do not hold MISO low (which would block SD card disk_initialize wait_ready). */
    gpio_init(SDCARD_PIN_SPI0_CS);
    gpio_set_dir(SDCARD_PIN_SPI0_CS, GPIO_OUT);
    gpio_put(SDCARD_PIN_SPI0_CS, 1);

#if TOUCH_PIN_CS >= 0
    /* XPT2046 touch controller CS — must not float */
    gpio_init(TOUCH_PIN_CS);
    gpio_set_dir(TOUCH_PIN_CS, GPIO_OUT);
    gpio_put(TOUCH_PIN_CS, 1);
#endif
#endif /* SHARED_SPI_BUS */

    // display = hagl_init();
    // hagl_clear(display);

    display_init();
    display_clear();
#ifdef ILI9341
    ili9341_infones_frame_timing_register_init();
    APU_Mute = 0;
#endif
#ifdef ST7789
    st7789_infones_frame_timing_register_init();
    APU_Mute = 0;
#endif

    // line_drawing=false;

    //
    // 
    // play samples in core1
    //
    // 
    // 735 samples per frame
    //
    /*
    *
    * AUDIO (not parallel process)
    *
    */
    #if 0
    #ifdef ILI9341
     audio_init(7,19654);
    #endif
    #ifdef ST7789
     audio_init(7,20050);
     // audio_init(7,22050);
     // audio_init(7,44100);
    #endif
    #endif

    //tusb_init();

    romSelector_.init(NES_FILE_ADDR);


    // util::dumpMemory((void *)NES_FILE_ADDR, 1024);

#if 0
    //
    auto *i2c = i2c0;
    static constexpr int I2C_SDA_PIN = 16;
    static constexpr int I2C_SCL_PIN = 17;
    i2c_init(i2c, 100 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    // gpio_pull_up(I2C_SDA_PIN);
    // gpio_pull_up(I2C_SCL_PIN);
    i2c_set_slave_mode(i2c, false, 0);

    {
        constexpr int addrSegmentPointer = 0x60 >> 1;
        constexpr int addrEDID = 0xa0 >> 1;
        constexpr int addrDisplayID = 0xa4 >> 1;

        uint8_t buf[128];
        int addr = 0;
        do
        {
            printf("addr: %04x\n", addr);
            uint8_t tmp = addr >> 8;
            i2c_write_blocking(i2c, addrSegmentPointer, &tmp, 1, false);

            tmp = addr & 255;
            i2c_write_blocking(i2c, addrEDID, &tmp, 1, true);
            i2c_read_blocking(i2c, addrEDID, buf, 128, false);

            util::dumpMemory(buf, 128);
            printf("\n");

            addr += 128;
        } while (buf[126]); 
    }
#endif
#if 0
    //
    dvi_ = std::make_unique<dvi::DVI>(pio0, &DVICONFIG,
                                      dvi::getTiming640x480p60Hz());
    //    dvi_->setAudioFreq(48000, 25200, 6144);
    dvi_->setAudioFreq(44100, 28000, 6272);
    dvi_->allocateAudioBuffer(256);
    //    dvi_->setExclusiveProc(&exclProc_);

    dvi_->getBlankSettings().top = 4 * 2;
    dvi_->getBlankSettings().bottom = 4 * 2;
    // dvi_->setScanLine(true);

    applyScreenMode();

    // 空サンプル詰めとく
    dvi_->getAudioRingBuffer().advanceWritePointer(255);
#endif
#if !defined(DISABLE_AUDIO) && !defined(I2S_AUDIO)
    /* PWM audio keeps core1 for the whole run: audio_init() claims a DMA
     * channel and an IRQ handler, so it is not written to be re-entered.
     * I2S starts core1 per game instead — see audio_core_start(). */
    multicore_launch_core1(core1_main);
#endif

    // InfoNES_Main();

    bool sdOk = false;
#if SDCARD_PIN_SPI0_CS >= 0
    sdOk = initSDCard();
#endif
#ifdef FLASHFS_ENABLED
    if (!sdOk) {
        flashFsActive = initFlashFS();
    }
#endif
    isFatalError = !sdOk && !flashFsActive;

    // When a game is started from the menu (SD-card mode), the menu reboots
    // the device and after reboot we read ROMINFOFILE to launch the chosen ROM.
    // Flash mode doesn't reboot (read-only filesystem can't persist the choice,
    // and DISABLE_AUDIO removes the audio-restart reason for rebooting), so we
    // skip the file read on watchdog-induced boots that landed on flashfs.
    if (watchdog_caused_reboot() && !isFatalError && !flashFsActive)
    {
        // Determine loaded rom
        printf("Rebooted by menu\n");
        FIL fil;
        FRESULT fr;
        size_t tmpSize;
        printf("Reading current game from %s and starting emulator\n", ROMINFOFILE);
        fr = f_open(&fil, ROMINFOFILE, FA_READ);
        if (fr == FR_OK)
        {
            size_t r;
            fr = f_read(&fil, selectedRom, sizeof(selectedRom), &r);        
            if (fr != FR_OK)
            {
                snprintf(ErrorMessage, 40, "Cannot read %s:%d\n", ROMINFOFILE, fr);
                selectedRom[0] = 0;
                printf(ErrorMessage);
            } else {
                selectedRom[r] = 0;
            }
        }
        else
        {
            snprintf(ErrorMessage, 40, "Cannot open %s:%d\n", ROMINFOFILE, fr);
            printf(ErrorMessage);
        }
        f_close(&fil);
    }
    while (true)
    {
        if (strlen(selectedRom) == 0)
        {
            screenMode_ = ScreenMode::NOSCANLINE_8_7;
            applyScreenMode();
            // try ROM if fatal error
            if(isFatalError){
                romSelector_.init(NES_FILE_ADDR);
                AUDIO_CORE_START();
                InfoNES_Main();
                AUDIO_CORE_STOP();
                /* InfoNES_Main returns when the player quits or switches ROM
                 * via SELECT+LEFT/RIGHT (romSelector_ has already advanced
                 * selectedIndex_). Loop back to run the next ROM rather than
                 * falling into the SD-based menu, which can't do anything
                 * useful in fatal mode. */
                continue;
            }
            menu(NES_FILE_ADDR, ErrorMessage, isFatalError);  // never returns, but reboots upon selecting a game
        }
        printf("Now playing: %s\n", selectedRom);
        romSelector_.init(NES_FILE_ADDR);
        AUDIO_CORE_START();
        InfoNES_Main();
        AUDIO_CORE_STOP();
        selectedRom[0] = 0;
    }

    return 0;
}
