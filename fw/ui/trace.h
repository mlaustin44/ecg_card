/* Sweep trace widgets (internal to fw/ui; exposed for tests).
 *
 * Each trace keeps one min/max envelope per screen column in normalised
 * units 0..255 (255 = top of the trace area), so a column costs 2 bytes and
 * drawing is one vertical span per column at any trace height. The pen
 * advances with sample count (Bresenham), so one sweep is exactly
 * fs * sweep_ms / 1000 samples. The column TRACE_GAP ahead of the pen is
 * cleared as the pen enters each column, which leaves the erase bar.
 *
 * Vertical scale is applied when a sample is stored, and only changes when
 * the pen wraps, so the old trace to the right of the pen stays as drawn.
 *   ECG: 1-2-5 steps of microvolts per trace height, with hysteresis.
 *   PPG: free range from the last sweep's peak-to-peak, with hysteresis. */
#ifndef UI_TRACE_H
#define UI_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "ui_model.h"

#define TRACE_W   OLED_W
#define TRACE_GAP 6 /* erase bar width, columns */

typedef struct {
    uint8_t lo[TRACE_W], hi[TRACE_W]; /* lo > hi: empty column */
    uint32_t acc, n_sweep;            /* pen phase: acc += TRACE_W per sample */
    int32_t center;                   /* raw value drawn at mid height */
    int32_t range;                    /* raw units per full trace height */
    int32_t k;                        /* (256 << 16) / range */
    int16_t smin, smax;               /* raw extremes of the current sweep */
    uint16_t pen;                     /* column being written */
    uint16_t wraps;                   /* sweeps completed since reset */
    uint8_t step;                     /* ECG: index into the 1-2-5 table */
    uint8_t down;                     /* hysteresis counter */
    uint8_t clipped;                  /* PPG first sweep: a sample clipped */
    bool col_has;                     /* pen column holds a sample */
    bool any;                         /* smin/smax valid */
} trace_state_t;

const trace_state_t *trace_state(ui_trace_t t);

/* ECG scale table, microvolts per full trace height. */
#define ECG_STEPS 5
extern const uint16_t trace_ecg_steps_uv[ECG_STEPS];
#define ECG_STEP_DEFAULT 2 /* 2 mV per trace height */

/* Scale bar for the ECG trace: value (uV) of the largest 1-2-5 value not
 * above half the current range, and its length in px at trace height h. */
uint16_t trace_bar_uv(void);
int trace_bar_px(int h);

/* Draw trace t into rows y0 .. y0+h-1 across the full width, 2 x 2 pen. */
void trace_draw(ui_trace_t t, int y0, int h);

#endif
