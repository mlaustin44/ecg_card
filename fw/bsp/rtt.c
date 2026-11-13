/* Minimal SEGGER-RTT-compatible control block (written from the published
 * layout, no SEGGER code). The J-Link finds it by scanning RAM for the
 * "SEGGER RTT" id, or by the ELF symbol _SEGGER_RTT.
 *   up 0   "Terminal"  1 KB  text log      (hal_log_write)
 *   up 1   "stream"    2 KB  binary frames (hal_stream_write)
 *   down 0 "Terminal"  16 B  (host -> target, unused by the HAL)
 * Mode: non-blocking, skip when full. Each call is all-or-nothing, so a
 * stream frame is never torn. Writers are main-loop only (hal.h). */
#include <string.h>

#include "bsp_internal.h"

#define RTT_MODE_NO_BLOCK_SKIP 0u
#define RTT_UP0_SIZE   1024u
#define RTT_UP1_SIZE   2048u
#define RTT_DOWN0_SIZE 16u

typedef struct {
    const char *sName;
    char *pBuffer;
    unsigned SizeOfBuffer;
    volatile unsigned WrOff; /* written by the target */
    volatile unsigned RdOff; /* written by the host */
    unsigned Flags;
} rtt_buffer_t;

typedef struct {
    char acID[16];
    int MaxNumUpBuffers;
    int MaxNumDownBuffers;
    rtt_buffer_t aUp[2];
    rtt_buffer_t aDown[1];
} rtt_cb_t;

/* Layout check against the SEGGER definition on a 32-bit target. */
_Static_assert(sizeof(rtt_buffer_t) == 24u, "RTT buffer descriptor layout");
_Static_assert(sizeof(rtt_cb_t) == 16u + 8u + 3u * 24u, "RTT control block layout");

rtt_cb_t _SEGGER_RTT __attribute__((aligned(4)));
static char s_up0[RTT_UP0_SIZE];
static char s_up1[RTT_UP1_SIZE];
static char s_down0[RTT_DOWN0_SIZE];

void bsp_rtt_init(void)
{
    static const char name_term[] = "Terminal";
    static const char name_stream[] = "stream";
    rtt_cb_t *cb = &_SEGGER_RTT;

    memset(cb, 0, sizeof *cb);
    cb->MaxNumUpBuffers = 2;
    cb->MaxNumDownBuffers = 1;
    cb->aUp[0] = (rtt_buffer_t){name_term, s_up0, RTT_UP0_SIZE, 0u, 0u,
                                RTT_MODE_NO_BLOCK_SKIP};
    cb->aUp[1] = (rtt_buffer_t){name_stream, s_up1, RTT_UP1_SIZE, 0u, 0u,
                                RTT_MODE_NO_BLOCK_SKIP};
    cb->aDown[0] = (rtt_buffer_t){name_term, s_down0, RTT_DOWN0_SIZE, 0u, 0u,
                                  RTT_MODE_NO_BLOCK_SKIP};
    /* Write the id last, tail first, so a host scanning RAM never matches a
     * half-initialised block, and so the complete id string never exists as
     * a constant elsewhere in RAM. */
    __DMB();
    memcpy(&cb->acID[7], "RTT", 4u);
    __DMB();
    memcpy(&cb->acID[0], "SEGGER", 6u);
    __DMB();
    cb->acID[6] = ' ';
    __DMB();
}

static size_t rtt_write(rtt_buffer_t *b, const uint8_t *src, size_t n)
{
    unsigned wr, rd, size, avail, first;

    if (src == NULL || n == 0u) return 0u;
    size = b->SizeOfBuffer;
    wr = b->WrOff;
    rd = b->RdOff;
    if (wr >= size || rd >= size) return 0u; /* host corrupted the block */
    avail = (rd > wr) ? (rd - wr - 1u) : (size - wr + rd - 1u);
    if (n > avail) return 0u; /* skip: all or nothing */

    first = size - wr;
    if (first > n) first = (unsigned) n;
    memcpy(&b->pBuffer[wr], src, first);
    if (n > first) memcpy(&b->pBuffer[0], src + first, n - first);
    wr += (unsigned) n;
    if (wr >= size) wr -= size;
    __DMB(); /* data visible before the host sees the new WrOff */
    b->WrOff = wr;
    return n;
}

size_t hal_log_write(const char *s, size_t n)
{
    return rtt_write(&_SEGGER_RTT.aUp[0], (const uint8_t *) s, n);
}

size_t hal_stream_write(const uint8_t *buf, size_t n)
{
    return rtt_write(&_SEGGER_RTT.aUp[1], buf, n);
}
