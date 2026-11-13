/* Standard GATT services of the ECG Card (link_fmt.h, "Standard services"):
 *
 *   Device Information 0x180A: Manufacturer, Model, Serial (BLE address),
 *     Firmware Revision (radio), Software Revision (MCU, LINK_RC_SET_INFO)
 *   Battery 0x180F: Battery Level 0x2A19 read + notify
 *   Heart Rate 0x180D: Heart Rate Measurement 0x2A37 notify
 *
 * Written in the style of WCH's devinfoservice.c, battservice.c and
 * heartrateservice.c (HeartRate example), reduced to what the contract
 * lists. */
#ifndef STD_SERVICES_H
#define STD_SERVICES_H

#include "CONFIG.h"

#define DIS_STR_MAX 20u

typedef void (*stdCccdChangeCB_t)(void);

/* ---- Device Information ---- */
bStatus_t Dis_AddService(void);
/* Strings are copied (truncated to DIS_STR_MAX), not terminated on air. */
void Dis_SetSerial(const char *s);
void Dis_SetSoftwareRev(const uint8_t *p, uint8_t len);

/* ---- Battery ---- */
bStatus_t Bas_AddService(stdCccdChangeCB_t cccdCB);
void Bas_SetLevel(uint8_t level);
uint8_t Bas_GetLevel(void);
uint16_t Bas_GetCccd(uint16_t connHandle);
bStatus_t Bas_Notify(uint16_t connHandle, attHandleValueNoti_t *pNoti);

/* ---- Heart Rate ---- */
bStatus_t Hrs_AddService(stdCccdChangeCB_t cccdCB);
uint16_t Hrs_GetCccd(uint16_t connHandle);
bStatus_t Hrs_Notify(uint16_t connHandle, attHandleValueNoti_t *pNoti);

/* Clear this connection's Battery and Heart Rate CCCDs (call on
 * GAP_LINK_TERMINATED_EVENT). */
void Std_Disconnected(uint16_t connHandle);

#endif
