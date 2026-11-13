/* Byte-at-a-time COBS/CRC frame receiver. Portable C (no WCH headers). */
#include "rl_parser.h"

#include <string.h>

void rl_parser_init(rl_parser_t *p)
{
    memset(p, 0, sizeof *p);
}

bool rl_parser_feed(rl_parser_t *p, uint8_t byte, rl_frame_t *out)
{
    size_t n;
    rl_frame_status_t s;

    if (byte != 0u) {
        if (p->overflow)
            return false;
        if (p->n >= sizeof p->enc) {
            p->overflow = true;
            p->st.oversize++;
            return false;
        }
        p->enc[p->n++] = byte;
        return false;
    }

    /* delimiter */
    if (p->overflow) {
        p->overflow = false;
        p->n = 0;
        return false;
    }
    if (p->n == 0u) {
        p->st.empty++;
        return false;
    }
    n = rl_cobs_decode(p->enc, p->n, p->dec, sizeof p->dec);
    p->n = 0;
    if (n == RL_COBS_ERROR) {
        p->st.cobs_err++;
        return false;
    }
    s = rl_frame_parse(p->dec, n, out);
    if (s == RL_FRAME_CRC) {
        p->st.crc_err++;
        return false;
    }
    if (s != RL_FRAME_OK) {
        p->st.len_err++;
        return false;
    }
    if (p->have_seq && out->seq != p->next_seq)
        p->st.seq_gaps += (uint8_t)(out->seq - p->next_seq);
    p->have_seq = true;
    p->next_seq = (uint8_t)(out->seq + 1u);
    p->st.frames++;
    return true;
}
