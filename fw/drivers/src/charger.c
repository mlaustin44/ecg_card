#include "charger.h"

#include "hal.h"

/* STAT toggle detector: consecutive debounced edges spaced 100..400 ms apart
 * form a series (2 Hz blink = 250 ms half period). */
#define EDGE_MIN_MS 100u
#define EDGE_MAX_MS 400u
#define STALE_MS    600u

static struct {
    bool inited;
    bool usb;           /* debounced */
    bool stat_high;     /* debounced */
    bool pg_pend, stat_pend;
    uint32_t pg_t, stat_t;
    bool have_edge;
    uint32_t last_edge, series_start;
    unsigned edges;
    bool fault;
    charger_state_t state;
} s;

static bool debounce(bool raw, bool *stable, bool *pend, uint32_t *t,
                     uint32_t now, uint32_t ms)
{
    if (raw == *stable) {
        *pend = false;
        return false;
    }
    if (!*pend) {
        *pend = true;
        *t = now;
    }
    if ((uint32_t)(now - *t) >= ms) {
        *stable = raw;
        *pend = false;
        return true;
    }
    return false;
}

static void reset_series(void)
{
    s.have_edge = false;
    s.edges = 0;
}

void charger_init(void)
{
    s.inited = false;
    s.fault = false;
    s.state = CHARGER_NO_USB;
    s.usb = false;
    s.stat_high = true;
    reset_series();
}

bool charger_tick(uint32_t now)
{
    bool usb_raw = !hal_pin_read(PIN_CHG_PG);
    bool stat_raw = hal_pin_read(PIN_CHG_STAT1);

    if (!s.inited) {
        s.inited = true;
        s.usb = usb_raw;
        s.stat_high = stat_raw;
        s.pg_pend = s.stat_pend = false;
        reset_series();
    } else {
        if (debounce(usb_raw, &s.usb, &s.pg_pend, &s.pg_t, now,
                     CHARGER_PG_DEBOUNCE_MS)) {
            reset_series();
            if (!s.usb) {
                s.fault = false;
            }
        }
        if (debounce(stat_raw, &s.stat_high, &s.stat_pend, &s.stat_t, now,
                     CHARGER_STAT_DEBOUNCE_MS) && s.usb) {
            uint32_t dt = (uint32_t)(now - s.last_edge);
            if (s.have_edge && dt >= EDGE_MIN_MS && dt <= EDGE_MAX_MS) {
                s.edges++;
            } else {
                s.series_start = now;
                s.edges = 1;
            }
            s.have_edge = true;
            s.last_edge = now;
        }
    }

    /* "recent" = a STAT edge within STALE_MS: any STAT change is held back
     * that long, since it may be the start of a fault blink. */
    bool recent = s.usb && s.have_edge && (uint32_t)(now - s.last_edge) <= STALE_MS;
    bool toggling = recent && s.edges >= 2;
    if (toggling && (uint32_t)(now - s.series_start) > CHARGER_FAULT_MS) {
        s.fault = true;
    }

    charger_state_t ns;
    if (!s.usb) {
        ns = CHARGER_NO_USB;
    } else if (s.fault) {
        ns = CHARGER_FAULT;
    } else if (recent && s.state != CHARGER_NO_USB) {
        ns = s.state; /* hold while the blink pattern is being assessed */
    } else {
        ns = s.stat_high ? CHARGER_COMPLETE : CHARGER_CHARGING;
    }
    bool changed = ns != s.state;
    s.state = ns;
    return changed;
}

charger_state_t charger_state(void) { return s.state; }
bool charger_usb_present(void) { return s.usb; }

const char *charger_state_name(charger_state_t st)
{
    switch (st) {
    case CHARGER_NO_USB: return "NO_USB";
    case CHARGER_CHARGING: return "CHARGING";
    case CHARGER_COMPLETE: return "COMPLETE";
    case CHARGER_FAULT: return "FAULT";
    }
    return "?";
}
