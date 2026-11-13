/* View model: everything the UI needs to draw one frame. The application
 * fills it; fw/ui renders it. The UI owns no application state and makes no
 * decisions - navigation and measurement logic live in fw/app.
 *
 * Buttons (handled by the app, shown here so the UI can draw hints):
 *   BUTTON1 short  enter / start the shown page; in Settings: change value;
 *                  during a measurement: end it and show the summary
 *   BUTTON2 short  next page; in Settings: next item
 *   BUTTON2 long   back to Home
 *   BUTTON1 long   power off (2 s)
 *   both, on About: Diagnostics
 *   Diagnostics RADIO page: BUTTON1 short toggles radio program mode
 * The thumbs cannot reach the buttons during a measurement, so measurement
 * screens are hands-free: they start on contact and end when thumbs lift. */
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
    UI_SCREEN_SETTINGS,
    UI_SCREEN_ABOUT,
    /* entered from a launcher card */
    UI_SCREEN_ECG,          /* live ECG */
    UI_SCREEN_PPG,          /* live PPG */
    UI_SCREEN_BOTH,         /* live ECG + PPG */
    UI_SCREEN_SUMMARY,      /* after a measurement */
    UI_SCREEN_DIAG,         /* hidden bring-up page */
    UI_SCREEN_COUNT
} ui_screen_t;

#define UI_CAROUSEL_PAGES 6 /* HOME .. ABOUT */

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

typedef enum { UI_MODE_ECG, UI_MODE_PPG, UI_MODE_BOTH } ui_mode_t;

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

    /* status bar */
    uint8_t batt_pct;        /* 0..100 */
    bool batt_valid;         /* false until the gauge has been read */
    bool usb_present;
    bool charging;
    bool charge_fault;
    bool time_valid;         /* wall clock set (by the phone, LINK_CMD_SET_TIME) */
    uint8_t hour, minute;
    ui_ble_t ble_state;

    /* UI_SCREEN_ECG / PPG / BOTH */
    ui_live_t live;

    /* UI_SCREEN_SUMMARY */
    ui_summary_t summary;

    /* UI_SCREEN_SETTINGS */
    settings_t settings;
    uint8_t settings_cursor; /* settings_item_t */

    /* UI_SCREEN_ABOUT */
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

#endif
