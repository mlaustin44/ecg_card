/* Device name, serial string and advertising payloads (portable, no WCH
 * headers). AD structure types are the Bluetooth assigned numbers. */
#ifndef RL_BLE_FMT_H
#define RL_BLE_FMT_H

#include <stddef.h>
#include <stdint.h>

#define RL_ADV_MAX          31u /* legacy advertising / scan response */
#define RL_NAME_PREFIX      "ECG Card "
#define RL_NAME_LEN         13u /* "ECG Card XXXX" */
#define RL_SERIAL_LEN       12u /* "AABBCCDDEEFF" */

#define RL_AD_FLAGS         0x01u
#define RL_AD_UUID16_ALL    0x03u
#define RL_AD_UUID128_ALL   0x07u
#define RL_AD_NAME_COMPLETE 0x09u
#define RL_AD_TX_POWER      0x0Au
#define RL_AD_CONN_INTERVAL 0x12u
#define RL_AD_FLAGS_LE_GENERAL_NO_BREDR 0x06u

/* "ECG Card XXXX": XXXX = last two bytes of the address as printed
 * (addr[1], addr[0]; addr is least significant byte first), upper-case
 * hex. out gets RL_NAME_LEN chars plus a terminator. */
void rl_ble_name(const uint8_t addr[6], char out[RL_NAME_LEN + 1u]);

/* Address as printed, most significant byte first, upper-case hex, no
 * separators. out gets RL_SERIAL_LEN chars plus a terminator. */
void rl_ble_serial(const uint8_t addr[6], char out[RL_SERIAL_LEN + 1u]);

#define RL_UUID16_HEART_RATE 0x180Du

/* Advertising data: flags (LE general discoverable, BR/EDR not supported),
 * the complete list of 16-bit service UUIDs (Heart Rate 0x180D, so
 * heart-rate apps list the card) and the complete list of 128-bit service
 * UUIDs (the ECG Card service). Returns the length (25). */
size_t rl_ble_adv_data(uint8_t out[RL_ADV_MAX]);

/* Scan response: complete local name, peripheral connection interval
 * range and TX power. Returns the length, or 0 if it would exceed 31. */
size_t rl_ble_scan_rsp(const char *name, uint16_t int_min, uint16_t int_max, int8_t tx_dbm,
                       uint8_t out[RL_ADV_MAX]);

#endif
