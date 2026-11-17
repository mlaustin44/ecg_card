/* Heart Rate Measurement (0x2A37) state, like a chest strap (link_fmt.h,
 * "Standard services"). Portable C, no WCH headers.
 *
 * Fed with LINK_STATUS (mode, meas), the 0x2A37 subscription and
 * STREAM_EVENT payloads; tells the caller when a measurement is due and
 * builds it:
 *   flags u8 (bit1 contact detected, bit2 contact supported = 1, bit4 RR
 *   present; bit0 0 = u8 bpm), u8 bpm, then 0..9 u16 RR in 1/1024 s,
 *   oldest first.
 * Enabled = a mode is active (mode != LINK_MODE_IDLE) and 0x2A37 is
 * subscribed. While enabled a measurement is due RL_HRS_PERIOD_MS after the
 * previous one (or after enabling), and at once on a beat if none was sent
 * for RL_HRS_PERIOD_MS (or none since enabling).
 *
 * bpm: the latest beat accepted by rl_hr_select (R_PEAK, or PULSE when no
 * R_PEAK for RL_HR_ECG_HOLD_MS), 0 when none since the mode started or
 * none for RL_HRS_BPM_STALE_MS (whatever the contact state).
 * Contact: LINK_STATUS meas == RL_HRS_MEAS_RUNNING. RR: from consecutive
 * R_PEAK sample indices (500 Hz) while in contact; the first R_PEAK after
 * contact is (re)gained gives none, nor do intervals outside
 * RL_HRS_RR_MIN_MS..RL_HRS_RR_MAX_MS. Pending intervals are sent once each;
 * beyond RL_HRS_RR_MAX the oldest are dropped. Pending intervals are
 * discarded when contact is lost (a measurement with RR always says
 * contact) and when the measurement is enabled (no stale backlog). Times
 * are milliseconds from any free-running u32 clock (wraps are fine). */
#ifndef RL_HRS_H
#define RL_HRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rl_msg.h"

#define RL_HRS_PERIOD_MS    1000u
#define RL_HRS_BPM_STALE_MS 5000u /* bpm 0 after this long without a beat */
#define RL_HRS_RR_MAX       9u
#define RL_HRS_MAX_LEN      (2u + 2u * RL_HRS_RR_MAX) /* 20 = 23-byte MTU - 3 */
#define RL_HRS_RR_MIN_MS    250u
#define RL_HRS_RR_MAX_MS    2500u
#define RL_HRS_SAMPLE_HZ    500u /* R_PEAK sample index rate */
#define RL_HRS_MEAS_RUNNING 2u   /* ui_meas_state_t UI_MEAS_RUNNING */

#define RL_HRS_FLAG_CONTACT   0x02u /* sensor contact detected */
#define RL_HRS_FLAG_SUPPORTED 0x04u /* sensor contact supported */
#define RL_HRS_FLAG_RR        0x10u /* RR intervals present */

#define RL_HRS_NEVER 0xFFFFFFFFu /* rl_hrs_wait_ms: not enabled */

typedef struct {
    rl_hr_sel_t sel;
    bool active;     /* LINK_STATUS mode != idle */
    bool contact;    /* LINK_STATUS meas == running */
    bool subscribed; /* 0x2A37 notifications enabled */
    bool have_prev;  /* prev_idx is an R_PEAK in the current contact */
    bool sent;       /* a measurement went out since enabling */
    bool have_beat;  /* beat_ms is valid: an accepted beat in this mode */
    uint32_t beat_ms; /* last accepted beat (R_PEAK or PULSE fallback) */
    uint32_t prev_idx;
    uint32_t ref_ms; /* last measurement, or when enabled (sent false) */
    uint16_t bpm;
    uint8_t n_rr;
    uint16_t rr[RL_HRS_RR_MAX]; /* 1/1024 s, oldest first */
    uint32_t rr_overflow;       /* intervals dropped: more than RL_HRS_RR_MAX */
    uint32_t rr_rejected;       /* intervals outside the valid range */
} rl_hrs_t;

void rl_hrs_init(rl_hrs_t *h);

/* LINK_STATUS mode and meas (called for every LINK_STATUS; only changes
 * matter). A mode starting resets bpm, the source selection and RR. */
void rl_hrs_status(rl_hrs_t *h, uint8_t mode, uint8_t meas, uint32_t now_ms);

/* 0x2A37 subscription (connected and CCCD notify set). */
void rl_hrs_subscribed(rl_hrs_t *h, bool on, uint32_t now_ms);

bool rl_hrs_enabled(const rl_hrs_t *h);

/* A STREAM_EVENT payload. Returns true if a measurement should be sent now
 * (enabled, a beat was accepted, and none was sent for RL_HRS_PERIOD_MS or
 * since enabling). */
bool rl_hrs_event(rl_hrs_t *h, const uint8_t *p, size_t n, uint32_t now_ms);

/* Milliseconds until the periodic measurement is due (0: now), or
 * RL_HRS_NEVER when not enabled. */
uint32_t rl_hrs_wait_ms(const rl_hrs_t *h, uint32_t now_ms);

/* Builds the 0x2A37 value, consumes the pending RR intervals and restarts
 * the period at now_ms. bpm goes to 0 once the last beat is
 * RL_HRS_BPM_STALE_MS old. Returns the length (2 + 2 * RR count, <= 20). */
size_t rl_hrs_take(rl_hrs_t *h, uint32_t now_ms, uint8_t out[RL_HRS_MAX_LEN]);

/* R_PEAK sample-index difference (500 Hz) in 1/1024 s, rounded to nearest.
 * Returns false (out untouched) outside RL_HRS_RR_MIN_MS..RL_HRS_RR_MAX_MS. */
bool rl_hrs_rr_1024(uint32_t samples, uint16_t *out);

#endif
