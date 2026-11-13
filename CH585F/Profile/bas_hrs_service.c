/* Battery (0x180F) and Heart Rate (0x180D) services. Tables, CCCD handling
 * and notification follow WCH's HeartRate example (battservice.c,
 * heartrateservice.c) without the ADC measurement, HID report reference,
 * body sensor location and control point, which the contract does not
 * use. The value comes from the MCU (LINK_STATUS, STREAM_EVENT). */
#include "std_services.h"

/* ---- Battery ---------------------------------------------------------- */

#define BAS_LEVEL_VALUE_IDX 2

static const uint8_t basServUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(BATT_SERV_UUID),
                                                      HI_UINT16(BATT_SERV_UUID)};
static const uint8_t basLevelUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(BATT_LEVEL_UUID),
                                                       HI_UINT16(BATT_LEVEL_UUID)};
static const gattAttrType_t basService = {ATT_BT_UUID_SIZE, basServUUID};
static uint8_t basLevelProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t basLevel;
static gattCharCfg_t basLevelCccd[PERIPHERAL_MAX_CONNECTION];
static stdCccdChangeCB_t basCccdCB;

static gattAttribute_t basAttrTbl[] = {
    {{ATT_BT_UUID_SIZE, primaryServiceUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&basService},
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &basLevelProps},
    {{ATT_BT_UUID_SIZE, basLevelUUID}, GATT_PERMIT_READ, 0, &basLevel},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID}, GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0,
     (uint8_t *)basLevelCccd},
};

/* ---- Heart Rate ------------------------------------------------------- */

#define HRS_MEAS_VALUE_IDX 2

static const uint8_t hrsServUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(HEARTRATE_SERV_UUID),
                                                      HI_UINT16(HEARTRATE_SERV_UUID)};
static const uint8_t hrsMeasUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(HEARTRATE_MEAS_UUID),
                                                      HI_UINT16(HEARTRATE_MEAS_UUID)};
static const gattAttrType_t hrsService = {ATT_BT_UUID_SIZE, hrsServUUID};
static uint8_t hrsMeasProps = GATT_PROP_NOTIFY;
static uint8_t hrsMeas; /* notify only; never read */
static gattCharCfg_t hrsMeasCccd[PERIPHERAL_MAX_CONNECTION];
static stdCccdChangeCB_t hrsCccdCB;

static gattAttribute_t hrsAttrTbl[] = {
    {{ATT_BT_UUID_SIZE, primaryServiceUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&hrsService},
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hrsMeasProps},
    {{ATT_BT_UUID_SIZE, hrsMeasUUID}, 0, 0, &hrsMeas},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID}, GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0,
     (uint8_t *)hrsMeasCccd},
};

/* ---- shared callbacks ------------------------------------------------- */

static bStatus_t std_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                uint16_t *pLen, uint16_t offset, uint16_t maxLen,
                                uint8_t method)
{
    uint16_t uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);
    (void)connHandle;
    (void)maxLen;
    (void)method;

    /* Make sure it's not a blob operation (no attributes here are long) */
    if (offset > 0)
        return ATT_ERR_ATTR_NOT_LONG;
    if (uuid == BATT_LEVEL_UUID) {
        *pLen = 1;
        pValue[0] = basLevel;
        return SUCCESS;
    }
    *pLen = 0;
    return ATT_ERR_ATTR_NOT_FOUND;
}

static bStatus_t std_WriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                 uint16_t len, uint16_t offset, uint8_t method)
{
    uint16_t uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);
    bStatus_t status;
    (void)method;

    if (uuid != GATT_CLIENT_CHAR_CFG_UUID)
        return ATT_ERR_ATTR_NOT_FOUND;
    status = GATTServApp_ProcessCCCWriteReq(connHandle, pAttr, pValue, len, offset,
                                            GATT_CLIENT_CFG_NOTIFY);
    if (status == SUCCESS) {
        if (pAttr->pValue == (uint8_t *)basLevelCccd && basCccdCB)
            basCccdCB();
        else if (pAttr->pValue == (uint8_t *)hrsMeasCccd && hrsCccdCB)
            hrsCccdCB();
    }
    return status;
}

static gattServiceCBs_t stdCBs = {
    std_ReadAttrCB,  /* Read callback function pointer */
    std_WriteAttrCB, /* Write callback function pointer */
    NULL             /* Authorization callback function pointer */
};

/* Reset Client Char Config when the connection has dropped. Called by the
 * application on GAP_LINK_TERMINATED_EVENT, as WCH's HeartRate example
 * does (HeartRate_HandleConnStatusCB / Batt_HandleConnStatusCB). */
void Std_Disconnected(uint16_t connHandle)
{
    /* Make sure this is not loopback connection */
    if (connHandle != LOOPBACK_CONNHANDLE) {
        GATTServApp_InitCharCfg(connHandle, basLevelCccd);
        GATTServApp_InitCharCfg(connHandle, hrsMeasCccd);
    }
}

bStatus_t Bas_AddService(stdCccdChangeCB_t cccdCB)
{
    basCccdCB = cccdCB;
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, basLevelCccd);
    return GATTServApp_RegisterService(basAttrTbl, GATT_NUM_ATTRS(basAttrTbl),
                                       GATT_MAX_ENCRYPT_KEY_SIZE, &stdCBs);
}

void Bas_SetLevel(uint8_t level)
{
    basLevel = level;
}

uint8_t Bas_GetLevel(void)
{
    return basLevel;
}

uint16_t Bas_GetCccd(uint16_t connHandle)
{
    return GATTServApp_ReadCharCfg(connHandle, basLevelCccd);
}

bStatus_t Bas_Notify(uint16_t connHandle, attHandleValueNoti_t *pNoti)
{
    if (!(Bas_GetCccd(connHandle) & GATT_CLIENT_CFG_NOTIFY))
        return bleIncorrectMode;
    pNoti->handle = basAttrTbl[BAS_LEVEL_VALUE_IDX].handle;
    return GATT_Notification(connHandle, pNoti, FALSE);
}

bStatus_t Hrs_AddService(stdCccdChangeCB_t cccdCB)
{
    hrsCccdCB = cccdCB;
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, hrsMeasCccd);
    return GATTServApp_RegisterService(hrsAttrTbl, GATT_NUM_ATTRS(hrsAttrTbl),
                                       GATT_MAX_ENCRYPT_KEY_SIZE, &stdCBs);
}

uint16_t Hrs_GetCccd(uint16_t connHandle)
{
    return GATTServApp_ReadCharCfg(connHandle, hrsMeasCccd);
}

bStatus_t Hrs_Notify(uint16_t connHandle, attHandleValueNoti_t *pNoti)
{
    if (!(Hrs_GetCccd(connHandle) & GATT_CLIENT_CFG_NOTIFY))
        return bleIncorrectMode;
    pNoti->handle = hrsAttrTbl[HRS_MEAS_VALUE_IDX].handle;
    return GATT_Notification(connHandle, pNoti, FALSE);
}
