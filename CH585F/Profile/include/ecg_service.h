/* ECG Card custom GATT service (link_fmt.h, "ECG Card service"):
 *
 *   service  d5780001-8a28-493a-ad8d-6b741ba09ea7
 *   ECG      d5780002  notify          STREAM_ECG_BLOCK payload
 *   PPG      d5780003  notify          STREAM_PPG_BLOCK payload
 *   Events   d5780004  notify          STREAM_EVENT payload
 *   Status   d5780005  read, notify    LINK_STATUS payload (20 bytes)
 *   Control  d5780006  write (with response), 1..16 bytes -> LINK_CMD
 *   Log      d5780007  notify          device log text (<= 32-byte chunks)
 *
 * Written in the style of WCH's gattprofile.c / ble_uart_service.c. */
#ifndef ECG_SERVICE_H
#define ECG_SERVICE_H

#include "CONFIG.h"

#define ECS_CHAR_ECG    0u
#define ECS_CHAR_PPG    1u
#define ECS_CHAR_EVENTS 2u
#define ECS_CHAR_STATUS 3u
#define ECS_CHAR_LOG    4u
#define ECS_NUM_NOTIFY  5u

/* A CCCD of this service was written (subscriptions may have changed). */
typedef void (*ecsCccdChangeCB_t)(void);
/* The phone wrote 1..16 bytes to Control. */
typedef void (*ecsControlCB_t)(uint16_t connHandle, const uint8_t *pValue, uint16_t len);

bStatus_t Ecs_AddService(ecsCccdChangeCB_t cccdCB, ecsControlCB_t controlCB);

/* Update the Status value (LINK_STATUS_LEN bytes) returned by reads. */
void Ecs_SetStatus(const uint8_t *pValue);

/* CCCD value of characteristic ch (ECS_CHAR_*) for this connection. */
uint16_t Ecs_GetCccd(uint16_t connHandle, uint8_t ch);

/* Send pNoti (allocated with GATT_bm_alloc) on characteristic ch. Returns
 * bleIncorrectMode if that client has not enabled notifications; on any
 * status other than SUCCESS the caller frees pNoti->pValue. */
bStatus_t Ecs_Notify(uint16_t connHandle, uint8_t ch, attHandleValueNoti_t *pNoti);

/* Clear this connection's CCCDs (call on GAP_LINK_TERMINATED_EVENT). */
void Ecs_Disconnected(uint16_t connHandle);

#endif
