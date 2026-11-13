/* Mains analysis on the 500 Hz stream (before any notch).
 *
 * Two Goertzel bins (50 and 60 Hz) over consecutive 250-sample (0.5 s)
 * rectangular windows: both frequencies fall exactly on bins, so each bin is
 * blind to the other tone and to DC. Per window it reports the pickup at each
 * frequency as input-referred microvolts peak-to-peak (amplitude of the
 * matching sinusoid x 2), and keeps an automatic notch choice:
 *   - a frequency is "dominant" if its amplitude >= DSP_MAINS_FLOOR_UVPP and
 *     >= 2x the other one;
 *   - while undecided the first dominant window decides; afterwards a change
 *     needs 2 consecutive windows dominated by the other frequency.
 * The pickup amplitude is also the "mains residual" contact-quality metric
 * (what the hardware CMRR / right-leg drive left in the stream). */
#ifndef DSP_MAINS_H
#define DSP_MAINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSP_MAINS_FLOOR_UVPP 5u

typedef struct {
    int32_t s1[2], s2[2]; /* Goertzel state, [0] = 50 Hz, [1] = 60 Hz */
    int32_t pp_q28;       /* uVpp per |X| for the gain in use */
    uint16_t n;           /* samples in the current window */
    uint16_t uvpp[2];     /* last complete window */
    uint8_t sel_hz;       /* 0 = undecided, 50 or 60 */
    uint8_t cand_hz, cand_cnt;
    uint8_t windows;      /* completed windows, saturating */
} dsp_mains_t;            /* 32 bytes */

void dsp_mains_init(dsp_mains_t *m, int gain);

/* Feed stream samples. Returns true if at least one window completed. */
bool dsp_mains_push(dsp_mains_t *m, const int16_t *x, size_t n);

/* Pickup at 50 or 60 Hz in the last window, uVpp (0 before the first window). */
uint16_t dsp_mains_uvpp(const dsp_mains_t *m, uint8_t hz);

/* Larger of the two (the quality metric). */
uint16_t dsp_mains_residual_uvpp(const dsp_mains_t *m);

/* Automatic notch choice: 50, 60, or 0 while undecided. */
uint8_t dsp_mains_auto_hz(const dsp_mains_t *m);

#endif
