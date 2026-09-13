#ifndef I2S_OUTPUT_H_FILE
#define I2S_OUTPUT_H_FILE

/* I2S audio output (PIO + DMA ping-pong), an alternative to the PWM path in
 * audio.c for targets with an external I2S DAC — e.g. PICO_RESTOUCH with a
 * Waveshare Pico Audio shield on GP26/27/28 (DIN=26, BCK=27, LRCK=28).
 *
 * Borrowed from tiny_agi (tinyagi-rp2350/audio/i2s_output.c); the PIO program
 * is unchanged, the producer is fed from InfoNES' audio ring instead.
 *
 * Independent of the display: its own PIO state machine and DMA channels, so
 * it never touches the LCD's SPI or DMA channel.
 *
 * Samples are signed 16-bit mono, the same as InfoNES' audio ring carries on
 * this path, duplicated into both channels on the way into the I2S FIFO.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fill up to `count` mono signed 16-bit samples, return how many were
 * written. Any shortfall is handled by the caller. */
typedef int (*i2s_fill_fn)(int16_t *dst, int count);

/* Claims the PIO SM and DMA channels on the first call (later calls only
 * restart the stream) and starts playing. */
void i2s_output_init(int sample_rate, i2s_fill_fn fill);

/* Stop the stream: break the DMA chain, abort both channels, halt the state
 * machine. Call from core0 AFTER core1 has been reset — otherwise the chained
 * DMA keeps replaying the last two buffers forever. Safe to call when already
 * stopped; i2s_output_init() starts it again. */
void i2s_output_stop(void);

/* Call repeatedly from the core that owns audio. Refills whichever ping-pong
 * buffer just finished playing; returns immediately if neither has. */
void i2s_output_pump(void);

/* Read and clear the producer/consumer counters: buffers played since the last
 * call, and how many samples had to be padded because the ring ran dry.
 * `buffers` at the nominal rate is sample_rate/256 per second (86 at 22050). */
void i2s_output_get_stats(uint32_t *buffers, uint32_t *shortfall);

#ifdef __cplusplus
}
#endif

#endif /* I2S_OUTPUT_H_FILE */
