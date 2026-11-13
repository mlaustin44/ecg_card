/* ECG decimator: 4000 Hz ADC0 codes -> 500 Hz stream samples.
 *
 * 95-tap linear-phase equiripple FIR (Q15), 8:1, implemented as a transposed
 * polyphase: every raw sample adds into the 12 outputs it contributes to, so
 * no input history is stored (12 MACs per raw sample, 96 per output).
 *   passband 0-150 Hz: 0.087 dB p-p ripple; >= 46 dB attenuation from 250 Hz.
 * Output in stream units (STREAM_ECG_SCALE): s = (code - mid) * 8, saturated
 * to int16. Group delay 47 raw samples: stream sample m represents raw time
 * 8m - 40, i.e. t = m * 2 ms + DSP_ECG_STREAM_T0_US (dsp_delays.h).
 *
 * The first block primes the filter as if the past input had been constant
 * at the first sample, so there is no start-up ramp. */
#ifndef DSP_DECIM_H
#define DSP_DECIM_H

#include <stddef.h>
#include <stdint.h>

#define DSP_DECIM_BRANCH 12

typedef struct {
    int32_t acc[DSP_DECIM_BRANCH]; /* partial outputs in flight */
    int16_t mid;                   /* mid-scale code */
    uint8_t phase;                 /* raw samples into the current block, 0..7 */
    uint8_t head;                  /* acc[] slot of the next output */
    uint8_t primed;
} dsp_decim_t;                     /* 56 bytes */

/* Most outputs one push of n raw samples can produce. */
#define DSP_DECIM_MAX_OUT(n) ((n) / 8u + 1u)

/* mid_code: VMID_MON measurement in ADC codes (not assumed 2048). */
void dsp_decim_init(dsp_decim_t *d, uint16_t mid_code);

/* Feed n raw 12-bit codes (any n, any alignment); writes 0..DSP_DECIM_MAX_OUT(n)
 * stream samples to out and returns how many. Output m is produced by the
 * raw sample with index 8m + 7 (counting from init). */
size_t dsp_decim_push(dsp_decim_t *d, const uint16_t *raw, size_t n, int16_t *out);

#endif
