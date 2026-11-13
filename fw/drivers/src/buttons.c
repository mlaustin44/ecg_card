#include "buttons.h"

#include "board.h"
#include "hal.h"

#define QLEN 8u

typedef struct {
    bool down;     /* debounced */
    bool pending;  /* raw differs from down, timing the candidate */
    uint32_t cand_t;
    uint32_t press_t;
    bool long_fired;
    bool combo;    /* part of a pair press, or press made while other held long */
    bool ignore;
    bool boot_held;
    bool stuck;
} btn_t;

static btn_t b[2];
static const board_pin_t k_pin[2] = {PIN_BUTTON1, PIN_BUTTON2};
static uint32_t s_boot_t, s_now, s_both_t;
static bool s_both_fired;
static btn_event_t q[QLEN];
static unsigned q_head, q_n;

static void push(btn_event_t e)
{
    if (q_n < QLEN) {
        q[(q_head + q_n) % QLEN] = e;
        q_n++;
    }
}

btn_event_t buttons_get_event(void)
{
    if (q_n == 0) {
        return BTN_EV_NONE;
    }
    btn_event_t e = q[q_head];
    q_head = (q_head + 1) % QLEN;
    q_n--;
    return e;
}

static bool raw_down(unsigned i) { return !hal_pin_read(k_pin[i]); }

void buttons_init(uint32_t now_ms)
{
    s_boot_t = s_now = now_ms;
    q_head = q_n = 0;
    s_both_fired = false;
    for (unsigned i = 0; i < 2; i++) {
        b[i] = (btn_t){0};
        if (raw_down(i)) {
            b[i].down = true;
            b[i].press_t = now_ms;
            b[i].ignore = true;
            b[i].boot_held = true;
        }
    }
}

void buttons_ignore_until_release(void)
{
    for (unsigned i = 0; i < 2; i++) {
        if (raw_down(i) || b[i].down) {
            if (!b[i].down) {
                b[i].down = true;
                b[i].press_t = s_now;
            }
            b[i].pending = false;
            b[i].ignore = true;
        }
    }
}

static void on_press(unsigned i)
{
    unsigned o = 1u - i;
    b[i].press_t = b[i].cand_t;
    b[i].long_fired = false;
    b[i].combo = false;
    if (b[o].down && !b[o].ignore && !b[o].stuck) {
        b[i].combo = true;
        if (!b[o].long_fired) {
            b[o].combo = true;
            s_both_t = b[i].press_t;
            s_both_fired = false;
        } else {
            s_both_fired = true; /* no pair event possible */
        }
    }
}

static void on_release(unsigned i)
{
    btn_t *x = &b[i];
    if (!x->ignore && !x->combo && !x->long_fired) {
        uint32_t held = (uint32_t)(x->cand_t - x->press_t);
        if (held < BTN_LONG_MS) {
            push(i == 0 ? BTN_EV_BTN1_SHORT : BTN_EV_BTN2_SHORT);
        }
    }
    x->ignore = false;
    x->boot_held = false;
    x->stuck = false;
    x->long_fired = false;
    x->combo = false;
}

void buttons_tick(uint32_t now)
{
    s_now = now;
    for (unsigned i = 0; i < 2; i++) {
        bool raw = raw_down(i);
        btn_t *x = &b[i];
        if (raw == x->down) {
            x->pending = false;
            continue;
        }
        if (!x->pending) {
            x->pending = true;
            x->cand_t = now;
        }
        if ((uint32_t)(now - x->cand_t) >= BTN_DEBOUNCE_MS) {
            x->pending = false;
            if (raw) {
                x->down = true;
                on_press(i);
            } else {
                on_release(i);
                x->down = false;
            }
        }
    }

    for (unsigned i = 0; i < 2; i++) {
        btn_t *x = &b[i];
        if (!x->down) {
            continue;
        }
        if (x->boot_held && !x->stuck && (uint32_t)(now - s_boot_t) > BTN_STUCK_MS) {
            x->stuck = true;
        }
        /* Held far longer than any deliberate press (stuck after boot,
         * pressed in a pocket): stuck too, so it stops counting as input. */
        if (!x->stuck && (uint32_t)(now - x->press_t) > BTN_HELD_MAX_MS) {
            x->stuck = true;
        }
        if (!x->ignore && !x->combo && !x->long_fired &&
            (uint32_t)(now - x->press_t) >= BTN_LONG_MS) {
            x->long_fired = true;
            push(i == 0 ? BTN_EV_BTN1_LONG : BTN_EV_BTN2_LONG);
        }
    }
    if (b[0].down && b[1].down && b[0].combo && b[1].combo && !s_both_fired &&
        (uint32_t)(now - s_both_t) >= BTN_LONG_MS) {
        s_both_fired = true;
        push(BTN_EV_BOTH_LONG);
    }
}

uint32_t buttons_hold_ms(btn_id_t id)
{
    const btn_t *x = &b[id];
    if (!x->down || x->ignore || x->stuck) {
        return 0;
    }
    if (x->combo) {
        if (b[0].down && b[1].down && b[0].combo && b[1].combo) {
            return (uint32_t)(s_now - s_both_t);
        }
        return 0;
    }
    return (uint32_t)(s_now - x->press_t);
}

bool buttons_any_down(void)
{
    return (b[0].down && !b[0].stuck) || (b[1].down && !b[1].stuck);
}

bool buttons_fault(void) { return b[0].stuck || b[1].stuck; }
