#include "i2s_output.h"
#include "audio_i2s.pio.h"        /* generated from audio_i2s.pio */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include <stdint.h>

#ifndef I2S_DATA_PIN
#define I2S_DATA_PIN 26            /* DIN */
#endif
#ifndef I2S_CLOCK_PIN_BASE
#define I2S_CLOCK_PIN_BASE 27      /* BCK=27, LRCK=28 (must be consecutive) */
#endif

/* Per-buffer sample count. The DMA read-address ring wraps every
 * I2S_NSAMPLES*4 bytes = 2^I2S_RING_BITS, so the two must stay in sync
 * (256 words = 1024 B = 2^10). */
#define I2S_NSAMPLES  256
#define I2S_RING_BITS 10

static PIO  i2s_pio = pio0;
static uint i2s_sm;
static uint i2s_offset;
static bool i2s_claimed = false;
static int  dma_a, dma_b;
static volatile int last_active = -1;
static i2s_fill_fn fill_cb;
static volatile uint32_t stat_buffers, stat_shortfall;

/* Aligned so the DMA read ring wraps within each buffer. */
static uint32_t buf_a[I2S_NSAMPLES] __attribute__((aligned(I2S_NSAMPLES * 4)));
static uint32_t buf_b[I2S_NSAMPLES] __attribute__((aligned(I2S_NSAMPLES * 4)));

/* Pull mono signed 16-bit samples from the source and duplicate each into both
 * I2S channels. A shortfall (the ring ran dry) holds the last sample rather
 * than jumping to silence — a step to silence and back is a click at the
 * buffer rate, which is the loudest part of an underrun. */
static void __not_in_flash_func(i2s_fill)(uint32_t *dst)
{
    static int16_t last = 0;
    int16_t mono[I2S_NSAMPLES];
    int n = fill_cb ? fill_cb(mono, I2S_NSAMPLES) : 0;
    if (n > 0) last = mono[n - 1];
    stat_buffers++;
    stat_shortfall += (uint32_t)(I2S_NSAMPLES - n);
    for (int i = n; i < I2S_NSAMPLES; i++) mono[i] = last;
    for (int i = 0; i < I2S_NSAMPLES; i++) {
        uint16_t s = (uint16_t)mono[i];
        dst[i] = ((uint32_t)s << 16) | s;
    }
}

/* The two DMA channels chain to each other (gapless ping-pong); this refills
 * whichever one just stopped playing, once per swap. A buffer is
 * I2S_NSAMPLES/rate long (~11.6 ms at 22050), so polling from the audio core's
 * loop catches every swap with room to spare. */
void __not_in_flash_func(i2s_output_pump)(void)
{
    if (dma_channel_is_busy(dma_a)) {
        if (last_active != dma_a) { last_active = dma_a; i2s_fill(buf_b); }
    } else if (dma_channel_is_busy(dma_b)) {
        if (last_active != dma_b) { last_active = dma_b; i2s_fill(buf_a); }
    }
}

void __not_in_flash_func(i2s_output_get_stats)(uint32_t *buffers, uint32_t *shortfall)
{
    if (buffers)   { *buffers   = stat_buffers;   stat_buffers = 0; }
    if (shortfall) { *shortfall = stat_shortfall; stat_shortfall = 0; }
}

static void i2s_config_channel(int chan, int chain_to, uint32_t *buf)
{
    dma_channel_config c = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&c, chain_to);
    channel_config_set_ring(&c, false, I2S_RING_BITS);   /* wrap read addr within the buffer */
    dma_channel_configure(chan, &c, &i2s_pio->txf[i2s_sm], buf, I2S_NSAMPLES, false);
}

/* Point a channel's CHAIN_TO at itself so aborting it does not trigger the
 * other one. al1_ctrl is the non-triggering alias, so this does not start
 * anything. */
static void i2s_break_chain(int chan)
{
    uint32_t ctrl = dma_hw->ch[chan].al1_ctrl;
    ctrl &= ~(0xfu << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
    ctrl |= ((uint32_t)chan) << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB;
    dma_hw->ch[chan].al1_ctrl = ctrl;
}

void i2s_output_stop(void)
{
    if (!i2s_claimed) return;

    i2s_break_chain(dma_a);
    i2s_break_chain(dma_b);
    dma_channel_abort(dma_a);
    dma_channel_abort(dma_b);

    pio_sm_set_enabled(i2s_pio, i2s_sm, false);
    pio_sm_clear_fifos(i2s_pio, i2s_sm);
}

void i2s_output_init(int sample_rate, i2s_fill_fn fill)
{
    fill_cb = fill;
    for (int i = 0; i < I2S_NSAMPLES; i++) { buf_a[i] = 0; buf_b[i] = 0; }

    if (!i2s_claimed) {
        i2s_sm = (uint)pio_claim_unused_sm(i2s_pio, true);
        pio_gpio_init(i2s_pio, I2S_DATA_PIN);
        pio_gpio_init(i2s_pio, I2S_CLOCK_PIN_BASE);
        pio_gpio_init(i2s_pio, I2S_CLOCK_PIN_BASE + 1);

        i2s_offset = pio_add_program(i2s_pio, &audio_i2s_program);
        audio_i2s_program_init(i2s_pio, i2s_sm, i2s_offset, I2S_DATA_PIN, I2S_CLOCK_PIN_BASE);

        dma_a = dma_claim_unused_channel(true);
        dma_b = dma_claim_unused_channel(true);
        i2s_claimed = true;
    } else {
        /* Restarting for the next game: the SM was halted mid-frame, so put it
         * back at the program's entry point with empty FIFOs and a fresh
         * clock divider phase. Claims are kept — re-claiming would leak. */
        i2s_output_stop();
        pio_sm_restart(i2s_pio, i2s_sm);
        pio_sm_clkdiv_restart(i2s_pio, i2s_sm);
        pio_sm_exec(i2s_pio, i2s_sm,
                    pio_encode_jmp(i2s_offset + audio_i2s_offset_entry_point));
    }

    /* Every call, not just the first: the rate changes between an NTSC game
     * and a PAL one. 64 PIO cycles per stereo frame (32 bits, BCLK toggles
     * twice per bit), so the 24.8 fixed-point divider is sysclk*4/rate. */
    uint32_t divider = clock_get_hz(clk_sys) * 4 / (uint32_t)sample_rate;
    pio_sm_set_clkdiv_int_frac(i2s_pio, i2s_sm, divider >> 8u, divider & 0xffu);

    i2s_config_channel(dma_a, dma_b, buf_a);
    i2s_config_channel(dma_b, dma_a, buf_b);

    pio_sm_set_enabled(i2s_pio, i2s_sm, true);
    dma_channel_start(dma_a);
}
