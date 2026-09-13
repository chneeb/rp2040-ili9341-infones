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
#include "NesRegion.h"
#ifdef I2S_AUDIO
#include "i2s_output.h"
#ifndef I2S_GAIN_PERCENT
#define I2S_GAIN_PERCENT 100
#endif

/* The 2A03's sound mixer.
 *
 * The five channels do not mix linearly on the real chip, and that is the
 * whole story behind percussion sounding harsh. Its two DACs are resistor
 * ladders whose output saturates:
 *
 *   pulse_out = 95.52  / (8128  / (p1 + p2)         + 100)
 *   tnd_out   = 163.67 / (24329 / (3*t + 2*n + d)   + 100)
 *
 * Triangle, noise and DPCM share the second one, so noise's contribution
 * shrinks as the other two rise. A linear sum has no such behaviour: it gives
 * noise its full small-signal weight all the time, and drums, hats and
 * explosions ride over the music. Weighting noise "correctly" for a linear
 * sum (x0.66 of a pulse, the small-signal slope of those same formulas) does
 * not fix it, and neither does any other single number — which is why x17,
 * x14 and x11 all sounded much the same.
 *
 * For contrast the Circle port sums the raw buffers, leaving noise at 1/17 of
 * a pulse purely because that is the ratio of their native ranges. It does not
 * sound harsh, but it is 11x quieter than the chip rather than right.
 *
 * Both DACs are small enough to tabulate — 31 and 203 entries, built at
 * compile time — so this costs two array reads per sample.
 *
 * InfoNES' buffers are recovered to the chip's own channel values first: a
 * pulse is 0x11 * vol and the triangle a 0..255 waveform (both map back by
 * x15/255), noise is already the 0..15 volume, and DPCM is the 7-bit level
 * halved, so it doubles back to 0..126. */
struct ApuMixTables
{
    int16_t pulse[31];      /* p1 + p2, each 0..15                */
    int16_t tnd[203];       /* 3*t + 2*n + d, t/n 0..15, d 0..126 */
};

static constexpr ApuMixTables makeApuMixTables()
{
    ApuMixTables t = {};
    /* Scaled so pulse_out + tnd_out at full scale is 32767. */
    for (int i = 1; i < 31; i++)
    {
        t.pulse[i] = (int16_t)(95.52 / (8128.0 / i + 100.0) * 32767.0 + 0.5);
    }
    for (int i = 1; i < 203; i++)
    {
        t.tnd[i] = (int16_t)(163.67 / (24329.0 / i + 100.0) * 32767.0 + 0.5);
    }
    return t;
}
static constexpr ApuMixTables apuMix = makeApuMixTables();

/* Noise trim, applied before the mixer. 100 is the chip — and the chip is
 * wrong here, for a reason that is not the mixer's fault: the noise channel's
 * LFSR clocks far above the 22050 Hz InfoNES renders at (pAPU_QUALITY 2), so
 * it is sampled far below its own rate and aliases into broadband hiss.
 * Correct amplitude, wrong spectrum — which at full level is heard as brushy
 * percussion over everything, and is why no amount of re-weighting the mix
 * fixed it. 10 is the level the Circle port arrives at by accident and the
 * value confirmed by ear here; raising pAPU_QUALITY would attack the cause
 * rather than the symptom, at double the APU cost on core0.
 * Per build: cmake .. -DAPU_MIX_NOISE_PERCENT=<n>. */
#ifndef APU_MIX_NOISE_PERCENT
#define APU_MIX_NOISE_PERCENT 10
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

/* One whole frame, handed to the panel in a single DMA.
 *
 * The per-scanline DMA this replaces could not reach 60 fps, and no amount of
 * frame-dropping could make it: 232 lines x 640 bytes at the 75 MHz the SPI
 * actually runs is 15.8 ms of pure transfer against a 16.67 ms budget, so the
 * bus has to be busy essentially all of the time — and it was not. Between
 * lines it sat idle while the code returned from a blocking wait, scaled 320
 * pixels and re-armed the channel. A few microseconds each, 232 times a frame,
 * is milliseconds gone, and they are the milliseconds that decide 60 fps.
 *
 * With the frame accumulated in RAM there is one transfer per frame: the bus
 * never goes idle, and it runs in the background while the next frame is
 * emulated. 320x232x2 = 145 KB each, which the RP2350's 520 KB can afford.
 *
 * One buffer, not two: a second costs 145 KB and there is not that much RAM
 * left. The tearing a single buffer invites is handled by wait_for_row_sent()
 * below, which costs nothing. */
#define NES_DRAWN_LINES (NES_LAST_SCANLINE - NES_FIRST_SCANLINE + 1)
static WORD frame_buf[DISPLAY_WIDTH * NES_DRAWN_LINES];

/* Row `line` of the frame buffer, for InfoNES to render straight into. */
static inline WORD *frame_row(int line)
{
    return frame_buf + (size_t)(line - NES_FIRST_SCANLINE) * DISPLAY_WIDTH;
}

static int display_dma_channel;

/* Hold the emulator off a row the transfer has not reached yet.
 *
 * With one buffer the next frame is rendered into the same memory the current
 * one is being sent from. A row takes 68 us to send and about 64 us to
 * emulate, so the emulator gains ~5 us a row: over 232 rows that is 1.1 ms
 * against the ~1.4 ms head start vblank gives the transfer. The margin is
 * thinner than the jitter, so now and again the writer overtakes the reader
 * near the bottom of the screen and part of the new frame appears inside the
 * old one — the occasional flicker.
 *
 * The DMA's read address says exactly where the transfer is, so rather than
 * buy a second buffer, wait out the overlap when it happens. It is a few
 * hundred microseconds once in a while, against 145 KB and a rebuild of the
 * memory map. */
static inline void __not_in_flash_func(wait_for_row_sent)(const WORD *row)
{
    const uint8_t *row_end = (const uint8_t *)(row + DISPLAY_WIDTH);
    while (dma_channel_is_busy(display_dma_channel)
           && (const uint8_t *)dma_hw->ch[display_dma_channel].read_addr < row_end)
    {
        tight_loop_contents();
    }
}
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
#define AUDIO_BUF_SIZE 1024
int buf_residue_size=AUDIO_BUF_SIZE;

#include "hardware/sync.h"

#define AUDIO_RING_BUFFER_SIZE 8192 // in samples

/* What one sample in the ring is. The PWM path wants what its DMA feeds the
 * slice's compare register: 8-bit unsigned, 128 = silence. The I2S DAC takes
 * signed 16-bit, so on that path the whole chain stays 16-bit and the APU mix
 * is no longer squeezed through a byte on the way out. */
#ifdef I2S_AUDIO
typedef int16_t audio_sample_t;
#define AUDIO_SILENCE 0
#else
typedef uint8_t audio_sample_t;
#define AUDIO_SILENCE 128
#endif

struct AudioRingBuffer {
    audio_sample_t buffer[AUDIO_RING_BUFFER_SIZE];
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

    void write(const audio_sample_t* data, int len) {
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
    int __not_in_flash_func(read)(audio_sample_t* dest, int max_len) {
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

#define TARGET_LATENCY_SAMPLES 1500 // Approx 2 frames of audio at 22050Hz

/* Scratch the mix is built in before going to the ring. */
static audio_sample_t audio_scratch[AUDIO_BUF_SIZE];

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
        while (audioRing.readable_size() > TARGET_LATENCY_SAMPLES)
        {
            sleep_us(100); 
        }

        int n = remaining;
        if (n > AUDIO_BUF_SIZE) n = AUDIO_BUF_SIZE;

        auto p = audio_scratch;
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
              * noise is only 0..15 (ApuC4Vol) and DPCM 0..63. Normalise each
              * through the 2A03's own two saturating DACs, which is what
              * keeps percussion in its place — see ApuMixTables above. */
             {
                 /* Back to the chip's own channel values, then through its two
                  * saturating DACs (see ApuMixTables above). */
                 int pulses = (w1 * 15 + 127) / 255 + (w2 * 15 + 127) / 255;
                 int tri    = (w3 * 15 + 127) / 255;
                 int noise  = (w4 * APU_MIX_NOISE_PERCENT) / 100;
                 int dpcm   = w5 * 2;
                 int tnd = 3 * tri + 2 * noise + dpcm;
                 /* Bounds: chip values cannot exceed these, but a noise trim
                  * above 100% can. */
                 if (pulses > 30) pulses = 30;
                 if (tnd > 202) tnd = 202;
                 int sum = apuMix.pulse[pulses] + apuMix.tnd[tnd];

                 /* The APU's signal is UNIPOLAR: silence is 0, and the DC level
                  * rides up and down with how many channels are sounding. Track
                  * it with a one-pole filter (shift 7 = ~27 Hz corner at 22050)
                  * and subtract, so the gain applies to the audio rather than
                  * to the offset — gaining about a fixed mid-point instead
                  * drives quiet passages into the rail and the output becomes a
                  * clipped square. */
                 static int32_t dc_acc = 0;    /* sum level, 24.8 fixed point */
                 dc_acc += (((int32_t)sum << 8) - dc_acc) >> 7;
                 int ac = sum - (dc_acc >> 8);

                 /* The tables already span 0..32767, so gain 100 is unity. */
                 int v = (ac * I2S_GAIN_PERCENT) / 100;
                 if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                 *p++ = (int16_t)v;
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
        audioRing.write(audio_scratch, n);
        
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
/* Region timing. InfoNES itself has no PAL support — the core always runs 262
 * scanlines — so a PAL ROM would otherwise be paced at the NTSC rate and play
 * about 20% fast, music included. The platform layer corrects most of that
 * with two numbers, exactly as the Circle port does.
 *
 * The sound rate is not optional and not obvious: the APU emits a fixed number
 * of samples per *emulated* frame, so pacing at 50 Hz produces five sixths as
 * many samples a second. Left at 22050 that is a permanent underrun; opening
 * the DAC at five sixths balances it AND fixes the pitch in one stroke, since
 * samples computed for 22050 played at 18350 come out a factor 0.8322 lower —
 * against the 0.8321 a PAL game wants.
 *
 * Still wrong afterwards: a PAL machine has 312 scanlines and correspondingly
 * more vblank, and this still has 262. Games that time raster effects to the
 * longer frame can misbehave. Fixing that means real PAL support in the core. */
#define NES_FRAME_PERIOD_NTSC_US 16639   /* 60.0988 Hz */
#define NES_FRAME_PERIOD_PAL_US  19997   /* 50.007 Hz  */
#define NES_AUDIO_RATE_NTSC      22050
#define NES_AUDIO_RATE_PAL       18350   /* five sixths — what the APU makes at 50 Hz */

static uint32_t nes_frame_period_us = NES_FRAME_PERIOD_NTSC_US;
static int      nes_audio_rate      = NES_AUDIO_RATE_NTSC;

/* Pick the pacing for the ROM about to run, from its 16 byte iNES header.
 * Detection believes only a NES 2.0 header (see NesRegion.h) — an undetected
 * PAL ROM behaves exactly as it did before, which is the safe way to be wrong.
 *
 * The audio rate has to follow the pacing or the output starves. Both sinks
 * can: the I2S device is opened per game, and the PWM rate is only a slice
 * clock divider (audio_set_rate), changeable at any time. */
static void applyRegionTiming(const uint8_t *rom)
{
    nes_frame_period_us = NES_FRAME_PERIOD_NTSC_US;
    nes_audio_rate      = NES_AUDIO_RATE_NTSC;

    enum TNesRegion region = rom ? NesRegionFromHeader(rom) : NesRegionNTSC;
    if (region == NesRegionPAL || region == NesRegionDendy)
    {
        nes_frame_period_us = NES_FRAME_PERIOD_PAL_US;
        nes_audio_rate      = NES_AUDIO_RATE_PAL;
    }

#ifndef I2S_AUDIO
    /* PWM: core1 was launched once at boot, so the rate is re-aimed in place.
     * Safe before core1 has run audio_init() — the rate is recorded and init
     * picks it up. */
    audio_set_rate(nes_audio_rate);
#endif

    if (rom)
    {
        printf("Region: %c — frame %lu us, audio %d Hz\n", NesRegionChar(region),
               (unsigned long)nes_frame_period_us, nes_audio_rate);
    }
}

/* Send the frame that has just been finished, unless the previous one is still
 * going out. Called from InfoNES_LoadFrame() at the start of vblank.
 *
 * Dropping only when the bus is genuinely still busy is the whole scheduling
 * policy now, and it needs no tuning: the transfer costs no CPU, so there is
 * nothing to trade against emulation and nothing to steer. Three attempts at
 * steering it — a frame deadline, a queue level, a cadence — are gone with the
 * per-line DMA that made them necessary. */
static void __not_in_flash_func(present_frame)(void)
{
    if (dma_channel_is_busy(display_dma_channel))
    {
        return;             /* previous frame still on the wire — drop this one */
    }

    /* Wait out the tail of the last transfer before touching the panel's
     * registers, then point it at the window and hold CS for the frame. */
    while (spi_is_busy(DISPLAY_SPI_PORT)) tight_loop_contents();
    display_set_address(0, NES_FIRST_SCANLINE, DISPLAY_WIDTH - 1, NES_LAST_SCANLINE);
    gpio_put(DISPLAY_PIN_DC, 1);
    gpio_put(DISPLAY_PIN_CS, 0);

    dma_channel_set_trans_count(display_dma_channel, sizeof(frame_buf), false);
    dma_channel_set_read_addr(display_dma_channel, frame_buf, true);
}

static void __not_in_flash_func(speed_control)(void)
{
  static uint64_t deadline = 0;

// frame timing control
  uint64_t cur_time = time_us_64();
  if (deadline == 0) deadline = cur_time;

  /* A plain rate cap. During play the audio ring throttle in
   * InfoNES_SoundOutput() is the real clock and this rarely has to wait; it is
   * what paces the menu and DISABLE_AUDIO targets.
   *
   * Lateness is never carried forward. The throttle paces every frame to the
   * DAC, so an accumulated offset can never be worked off, and a deadline that
   * remembers one is late for ever — which is how this froze the picture
   * twice. */
  if ((int64_t)(cur_time - deadline) <= 0)
  {
      while ((int64_t)(time_us_64() - deadline) < 0) tight_loop_contents();
  }
  else
  {
      deadline = cur_time;
  }
  deadline += nes_frame_period_us;

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
static int __not_in_flash_func(i2s_pull)(int16_t *dst, int count)
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
    i2s_output_init(nes_audio_rate, i2s_pull);

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
            memset(buf + n, AUDIO_SILENCE, AUDIO_BUFFER_SIZE - n);
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
    present_frame();

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
    wait_for_row_sent(frame_row(line));
    RomSelect_SetLineBuffer(frame_row(line), 256);
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
    /* Before InfoNES renders into this row, make sure the frame still going
     * out has already been read from it. */
    wait_for_row_sent(frame_row(line));

#if 0
    util::WorkMeterMark(0xaaaa);
    auto b = dvi_->getLineBuffer();
    util::WorkMeterMark(0x5555);
    InfoNES_SetLineBuffer(b->data() + 32, b->size());
    //    (*b)[319] = line + dvi_->getFrameCounter();

    currentLineBuffer_ = b;
#endif
    InfoNES_SetLineBuffer(frame_row(line), 256);
}

void __not_in_flash_func(InfoNES_PostDrawLine)(int line)
{
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
    /* Widen this row in place. Nothing is sent here any more — the whole frame
     * goes out in one transfer from InfoNES_LoadFrame(). */
    WORD *fb = frame_row(line);
#if DISPLAY_WIDTH == 320
    /* Scale NES 256px wide → 320px wide (nearest-neighbour, right-to-left in-place). */
    for (int i = 319; i >= 0; i--) fb[i] = fb[i * 256 / 320];
#else
    /* Crop NES 256px wide → 240px wide: drop 8px overscan on each side. */
    for (int i = 0; i < 240; i++) fb[i] = fb[i + 8];
#endif
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
                applyRegionTiming(romSelector_.getCurrentROM());
                AUDIO_CORE_START();
                InfoNES_Main();
                AUDIO_CORE_STOP();
                applyRegionTiming(nullptr);   /* menu runs at NTSC pacing */
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
        applyRegionTiming(romSelector_.getCurrentROM());
        AUDIO_CORE_START();
        InfoNES_Main();
        AUDIO_CORE_STOP();
        applyRegionTiming(nullptr);   /* menu runs at NTSC pacing */
        selectedRom[0] = 0;
    }

    return 0;
}
