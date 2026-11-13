/* Device name, serial string and advertising payloads. Portable C. */
#include "rl_ble_fmt.h"

#include <string.h>

#include "link_fmt.h"

static const char hexdig[] = "0123456789ABCDEF";

static char *put_hex(char *o, uint8_t b)
{
    *o++ = hexdig[b >> 4];
    *o++ = hexdig[b & 0x0Fu];
    return o;
}

void rl_ble_name(const uint8_t addr[6], char out[RL_NAME_LEN + 1u])
{
    char *o = out;
    memcpy(o, RL_NAME_PREFIX, sizeof RL_NAME_PREFIX - 1u);
    o += sizeof RL_NAME_PREFIX - 1u;
    o = put_hex(o, addr[1]);
    o = put_hex(o, addr[0]);
    *o = '\0';
}

void rl_ble_serial(const uint8_t addr[6], char out[RL_SERIAL_LEN + 1u])
{
    char *o = out;
    int i;
    for (i = 5; i >= 0; i--)
        o = put_hex(o, addr[i]);
    *o = '\0';
}

size_t rl_ble_adv_data(uint8_t out[RL_ADV_MAX])
{
    static const uint8_t uuid[16] = LINK_UUID128(LINK_UUID_SERVICE);
    size_t n = 0;
    out[n++] = 2u;
    out[n++] = RL_AD_FLAGS;
    out[n++] = RL_AD_FLAGS_LE_GENERAL_NO_BREDR;
    out[n++] = 17u;
    out[n++] = RL_AD_UUID128_ALL;
    memcpy(&out[n], uuid, sizeof uuid);
    n += sizeof uuid;
    return n;
}

size_t rl_ble_scan_rsp(const char *name, uint16_t int_min, uint16_t int_max, int8_t tx_dbm,
                       uint8_t out[RL_ADV_MAX])
{
    size_t len = strlen(name), n = 0;
    if (2u + len + 6u + 3u > RL_ADV_MAX)
        return 0;
    out[n++] = (uint8_t)(1u + len);
    out[n++] = RL_AD_NAME_COMPLETE;
    memcpy(&out[n], name, len);
    n += len;
    out[n++] = 5u;
    out[n++] = RL_AD_CONN_INTERVAL;
    out[n++] = (uint8_t)int_min;
    out[n++] = (uint8_t)(int_min >> 8);
    out[n++] = (uint8_t)int_max;
    out[n++] = (uint8_t)(int_max >> 8);
    out[n++] = 2u;
    out[n++] = RL_AD_TX_POWER;
    out[n++] = (uint8_t)tx_dbm;
    return n;
}
