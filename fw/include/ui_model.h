/* View model: everything the UI needs to draw one frame. The application
 * fills it; fw/ui renders it. The UI owns no application state and makes no
 * decisions - navigation and measurement logic live in fw/app.
 *
 * Buttons (handled by the app, shown here so the UI can draw hints):
 *   BUTTON1 short  enter / start the shown page; in Settings: change value;
 *                  during a measurement: end it and show the summary
 *   BUTTON2 short  next page; in Settings: next item; on the ECG and Pulse
 *                  live screens: Live view trace <-> trace + HR graph
 *   HR monitor     BUTTON1 short ends it (back to its card, no summary); while
 *                  the display is dimmed or dark any press only wakes it
 *   BUTTON2 long   back to Home
 *   BUTTON1 long   power off (2 s)
 *   both, on About: Diagnostics
 *   Diagnostics RADIO page: BUTTON1 short toggles radio program mode
 *   Boot screen: any press skips it to Home (and does nothing else);
 *   power-down screen: buttons ignored
 * The thumbs cannot reach the buttons during a measurement, so measurement
 * screens are hands-free: they start on contact and end when thumbs lift.
 * The HR monitor is the exception: like a chest strap taken off, lifting the
 * thumbs only pauses it until the next contact. */
#ifndef UI_MODEL_H
#define UI_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "settings.h"

typedef enum {
    /* carousel pages, in BUTTON2 order */
    UI_SCREEN_HOME,
    UI_SCREEN_ECG_CARD,     /* launcher card: "ECG - press to start" */
    UI_SCREEN_PPG_CARD,
    UI_SCREEN_BOTH_CARD,
    UI_SCREEN_HRM_CARD,     /* HR monitor: a Bluetooth heart-rate sensor for other apps */
    UI_SCREEN_SETTINGS,
    UI_SCREEN_ABOUT,
    /* entered from a launcher card */
    UI_SCREEN_ECG,          /* live ECG */
    UI_SCREEN_PPG,          /* live PPG */
    UI_SCREEN_BOTH,         /* live ECG + PPG */
    UI_SCREEN_HRM,          /* HR monitor: rate, contact, Bluetooth state (no trace) */
    UI_SCREEN_SUMMARY,      /* after a measurement */
    UI_SCREEN_DIAG,         /* hidden bring-up page */
    /* shown by the app for a fixed time, no buttons (screen_ms animates) */
    UI_SCREEN_BOOT,         /* after reset and after a button wake from STANDBY */
    UI_SCREEN_POWER_DOWN,   /* after BUTTON1 long, before STANDBY */
    UI_SCREEN_COUNT
} ui_screen_t;

#define UI_CAROUSEL_PAGES 7 /* HOME .. ABOUT */

/* Settings page cursor: UI_SET_CLOSED as the carousel shows the page (values
 * listed, BUTTON1 opens it); open, an item 0 .. SET_ITEM_COUNT - 1 or the
 * "Exit" row after the last one (BUTTON2 cycles round, BUTTON1 closes). */
#define UI_SET_EXIT   ((uint8_t)SET_ITEM_COUNT)
#define UI_SET_CLOSED 0xFFu

typedef enum {
    UI_MEAS_WAIT_CONTACT, /* armed, "place thumbs" */
    UI_MEAS_SETTLING,     /* contact made, front end settling */
    UI_MEAS_RUNNING,
    UI_MEAS_USB_BLOCKED,  /* ECG requested while USB power present */
    UI_MEAS_FAULT         /* analog self-check failed; see fault_text */
} ui_meas_state_t;

typedef enum {
    UI_OVERLAY_NONE,
    UI_OVERLAY_USB_STOPPED,   /* "Stopped: USB connected" */
    UI_OVERLAY_LOW_BATTERY,   /* < 5 %: measurements disabled */
    UI_OVERLAY_BATT_CRITICAL, /* shutting down */
    UI_OVERLAY_POWER_OFF,     /* BUTTON1 held: hold_progress 0..100 */
    UI_OVERLAY_FAULT          /* fault_text */
} ui_overlay_t;

/* UI_MODE_HRM: ECG acquisition for a standard Heart Rate (0x180D) session;
 * loss of contact waits for the next one instead of ending it. */
typedef enum { UI_MODE_ECG, UI_MODE_PPG, UI_MODE_BOTH, UI_MODE_HRM } ui_mode_t;

/* Bluetooth status icon (Home and the carousel title rows). */
typedef enum {
    UI_BLE_OFF,         /* setting Off, or radio off (advertising timeout, low battery): no icon */
    UI_BLE_ADVERTISING, /* powering up / advertising: blinking rune */
    UI_BLE_CONNECTED,   /* a phone is connected: steady rune */
    UI_BLE_FAULT        /* radio did not answer: rune with "!" */
} ui_ble_t;

/* Live values. 0 means "not available yet" for rates and times. */
typedef struct {
    ui_meas_state_t state;
    uint16_t hr_bpm;         /* from ECG R-peaks */
    uint16_t pulse_bpm;      /* from PPG */
    uint16_t pat_ms;         /* pulse arrival time */
    uint16_t pi_permille;    /* perfusion index, 0.1 % units */
    uint8_t  quality;        /* 0 (no/poor contact) .. 4 (clean) */
    uint8_t  beat_seq;       /* increments on every detected beat (heart blink) */
    bool     gain_hi;        /* ECG gain 1098x in use */
    uint8_t  notch_hz;       /* 50 or 60: notch actually in use */
    uint16_t elapsed_s;      /* time in UI_MEAS_RUNNING */
    uint8_t  settle_pct;     /* 0..100 during UI_MEAS_SETTLING */
    bool     hum;            /* ECG: strong mains pickup, "unplug cables" */
    uint16_t fast_bpm;       /* 2-beat rate for the HR-graph view, 0 = n/a */
} ui_live_t;

typedef struct {
    ui_mode_t mode;
    uint16_t duration_s;
    uint16_t beats;
    uint16_t hr_avg, hr_min, hr_max; /* bpm; 0 = n/a */
    uint16_t sdnn_ms, rmssd_ms;      /* HRV; 0 = n/a (too few beats) */
    uint16_t pulse_avg;              /* bpm from PPG; 0 = n/a */
    uint16_t pi_permille;            /* mean perfusion index; 0 = n/a */
    uint16_t pat_ms;                 /* mean PAT; 0 = n/a */
    bool     stopped_by_usb;
    bool     valid;                  /* false: too short to summarise */
} ui_summary_t;

#define UI_DIAG_LINES 6
#define UI_DIAG_COLS  21 /* 6-pixel cells across 128 px */

typedef struct {
    ui_screen_t screen;
    ui_overlay_t overlay;
    uint8_t hold_progress;   /* UI_OVERLAY_POWER_OFF */
    const char *fault_text;  /* UI_OVERLAY_FAULT / UI_MEAS_FAULT; may be NULL */
    uint32_t now_ms;         /* hal_millis(), for animations */
    uint16_t screen_ms;      /* UI_SCREEN_BOOT / POWER_DOWN: ms since it was entered */

    /* status bar */
    uint8_t batt_pct;        /* 0..100 */
    bool batt_valid;         /* false until the gauge has been read */
    bool usb_present;
    bool charging;
    bool charge_fault;
    bool time_valid;         /* wall clock set (by the phone, LINK_CMD_SET_TIME) */
    uint8_t hour, minute;
    ui_ble_t ble_state;
    bool ble_hr;             /* the connected client is subscribed to Heart Rate (0x2A37) */

    /* UI_SCREEN_ECG / PPG / BOTH / HRM */
    ui_live_t live;

    /* UI_SCREEN_SUMMARY */
    ui_summary_t summary;

    /* UI_SCREEN_SETTINGS */
    settings_t settings;
    uint8_t settings_cursor; /* settings_item_t, UI_SET_EXIT or UI_SET_CLOSED */

    /* UI_SCREEN_ABOUT, UI_SCREEN_BOOT */
    const char *fw_version;  /* e.g. "0.1.0" */
    const char *radio_fw;    /* radio firmware "1.2.3" once a HELLO arrived; NULL = n/a */
    const char *ble_id;      /* last 4 hex digits of the BLE address ("ECG Card XXXX"); NULL = unknown */
    uint16_t batt_mv;

    /* UI_SCREEN_DIAG: preformatted text, NUL-terminated lines */
    uint8_t diag_page, diag_pages;
    char diag[UI_DIAG_LINES][UI_DIAG_COLS + 1];
} ui_model_t;

/* ---- Rendering API (implemented by fw/ui) --------------------------------
 * Frame buffer format: OLED_FB_BYTES bytes in SSD1306 native layout.
 * Byte index = page * 128 + x, page = y / 8, bit (y % 8) with bit 0 the top
 * row of the page. 1 = pixel lit. */
void ui_init(void);

/* Draw one complete frame for the model into fb. */
void ui_render(const ui_model_t *m, uint8_t fb[OLED_FB_BYTES]);

/* Live traces (sweep with erase bar). The trace widgets keep their own
 * column history between frames.
 *   UI_TRACE_ECG: display-path samples at ECG_HZ, in microvolts.
 *   UI_TRACE_PPG: band-passed PPG at PPG_HZ, arbitrary units (auto-scaled).
 * ui_trace_reset() clears both and applies the sweep time; call it when a
 * measurement screen is entered and whenever contact is re-made. */
typedef enum { UI_TRACE_ECG, UI_TRACE_PPG, UI_TRACE_COUNT } ui_trace_t;
void ui_trace_reset(uint16_t sweep_time_ms);
void ui_trace_push(ui_trace_t trace, const int16_t *samples, uint16_t n);
/* n samples with no display value (the ECG front end briefly on a rail):
 * the pen advances over empty columns, so the sweep keeps time. */
void ui_trace_gap(ui_trace_t trace, uint16_t n);
/* ECG scale reference: median QRS peak-to-peak in uV (0 = not known yet) and
 * whether the R waves point down. Applied from the next sweep; kept by
 * ui_trace_reset() (set 0 at the start of a measurement). */
void ui_trace_ecg_ref(uint16_t qrs_uv, bool r_down);

/* Heart-rate graph (Live view "+HR graph"): the app pushes the fast rate
 * (acq fast_bpm; 0 = none: a gap) every UI_HRG_STEP_MS while measuring; the
 * graph shows the last UI_HRG_N points (about 32 s), newest on the right,
 * scaled to their range (at least 10 bpm). */
#define UI_HRG_STEP_MS 300u
#define UI_HRG_N       108u
void ui_hrgraph_reset(void);
void ui_hrgraph_push(uint16_t bpm);

#endif
