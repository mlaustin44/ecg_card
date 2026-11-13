/* ECG Card custom GATT service. Attribute table, CCCD handling and the
 * read/write callbacks follow WCH's Peripheral gattprofile.c and BLE_UART
 * ble_uart_service.c (128-bit UUIDs). */
#include "ecg_service.h"

#include "link_fmt.h"

/* Attribute table indices */
#define IDX_ECG_VALUE     2
#define IDX_ECG_CCCD      3
#define IDX_PPG_VALUE     5
#define IDX_PPG_CCCD      6
#define IDX_EVENTS_VALUE  8
#define IDX_EVENTS_CCCD   9
#define IDX_STATUS_VALUE  11
#define IDX_STATUS_CCCD   12
#define IDX_CONTROL_VALUE 14

static const uint8_t ecsServUUID[ATT_UUID_SIZE] = LINK_UUID128(LINK_UUID_SERVICE);
static const uint8_t ecsEcgUUID[ATT_UUID_SIZE] = LINK_UUID128(LINK_UUID_ECG);
static const uint8_t ecsPpgUUID[ATT_UUID_SIZE] = LINK_UUID128(LINK_UUID_PPG);
static const uint8_t ecsEventsUUID[ATT_UUID_SIZE] = LINK_UUID128(LINK_UUID_EVENTS);
static const uint8_t ecsStatusUUID[ATT_UUID_SIZE] = LINK_UUID128(LINK_UUID_STATUS);
static const uint8_t ecsControlUUID[ATT_UUID_SIZE] = LINK_UUID128(LINK_UUID_CONTROL);

static const gattAttrType_t ecsService = {ATT_UUID_SIZE, ecsServUUID};

static uint8_t ecsNotifyProps = GATT_PROP_NOTIFY;
static uint8_t ecsStatusProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t ecsControlProps = GATT_PROP_WRITE;

/* Notify-only values are never read; one dummy byte backs them. */
static uint8_t ecsDummy;
static uint8_t ecsStatus[LINK_STATUS_LEN];
static uint8_t ecsControl[LINK_CMD_MAX_LEN];

static gattCharCfg_t ecsEcgCccd[PERIPHERAL_MAX_CONNECTION];
static gattCharCfg_t ecsPpgCccd[PERIPHERAL_MAX_CONNECTION];
static gattCharCfg_t ecsEventsCccd[PERIPHERAL_MAX_CONNECTION];
static gattCharCfg_t ecsStatusCccd[PERIPHERAL_MAX_CONNECTION];

static ecsCccdChangeCB_t ecsCccdCB;
static ecsControlCB_t ecsControlCB;

static gattAttribute_t ecsAttrTbl[] = {
    /* 0 ECG Card service */
    {{ATT_BT_UUID_SIZE, primaryServiceUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&ecsService},

    /* 1..3 ECG stream */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &ecsNotifyProps},
    {{ATT_UUID_SIZE, ecsEcgUUID}, 0, 0, &ecsDummy},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID}, GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0,
     (uint8_t *)ecsEcgCccd},

    /* 4..6 PPG stream */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &ecsNotifyProps},
    {{ATT_UUID_SIZE, ecsPpgUUID}, 0, 0, &ecsDummy},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID}, GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0,
     (uint8_t *)ecsPpgCccd},

    /* 7..9 Events */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &ecsNotifyProps},
    {{ATT_UUID_SIZE, ecsEventsUUID}, 0, 0, &ecsDummy},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID}, GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0,
     (uint8_t *)ecsEventsCccd},

    /* 10..12 Status */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &ecsStatusProps},
    {{ATT_UUID_SIZE, ecsStatusUUID}, GATT_PERMIT_READ, 0, ecsStatus},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID}, GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0,
     (uint8_t *)ecsStatusCccd},

    /* 13..14 Control */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &ecsControlProps},
    {{ATT_UUID_SIZE, ecsControlUUID}, GATT_PERMIT_WRITE, 0, ecsControl},
};

static bStatus_t ecs_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                uint16_t *pLen, uint16_t offset, uint16_t maxLen,
                                uint8_t method);
static bStatus_t ecs_WriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                 uint16_t len, uint16_t offset, uint8_t method);

static gattServiceCBs_t ecsCBs = {
    ecs_ReadAttrCB,  /* Read callback function pointer */
    ecs_WriteAttrCB, /* Write callback function pointer */
    NULL             /* Authorization callback function pointer */
};

static gattCharCfg_t *ecs_cccd_table(uint8_t ch)
{
    switch (ch) {
    case ECS_CHAR_ECG:
        return ecsEcgCccd;
    case ECS_CHAR_PPG:
        return ecsPpgCccd;
    case ECS_CHAR_EVENTS:
        return ecsEventsCccd;
    case ECS_CHAR_STATUS:
        return ecsStatusCccd;
    default:
        return NULL;
    }
}

static uint16_t ecs_value_handle(uint8_t ch)
{
    switch (ch) {
    case ECS_CHAR_ECG:
        return ecsAttrTbl[IDX_ECG_VALUE].handle;
    case ECS_CHAR_PPG:
        return ecsAttrTbl[IDX_PPG_VALUE].handle;
    case ECS_CHAR_EVENTS:
        return ecsAttrTbl[IDX_EVENTS_VALUE].handle;
    case ECS_CHAR_STATUS:
        return ecsAttrTbl[IDX_STATUS_VALUE].handle;
    default:
        return 0;
    }
}

bStatus_t Ecs_AddService(ecsCccdChangeCB_t cccdCB, ecsControlCB_t controlCB)
{
    ecsCccdCB = cccdCB;
    ecsControlCB = controlCB;

    /* Initialize Client Characteristic Configuration attributes */
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, ecsEcgCccd);
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, ecsPpgCccd);
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, ecsEventsCccd);
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, ecsStatusCccd);

    /* Register GATT attribute list and CBs with GATT Server App */
    return GATTServApp_RegisterService(ecsAttrTbl, GATT_NUM_ATTRS(ecsAttrTbl),
                                       GATT_MAX_ENCRYPT_KEY_SIZE, &ecsCBs);
}

void Ecs_SetStatus(const uint8_t *pValue)
{
    tmos_memcpy(ecsStatus, pValue, LINK_STATUS_LEN);
}

uint16_t Ecs_GetCccd(uint16_t connHandle, uint8_t ch)
{
    gattCharCfg_t *t = ecs_cccd_table(ch);
    return t ? GATTServApp_ReadCharCfg(connHandle, t) : 0;
}

bStatus_t Ecs_Notify(uint16_t connHandle, uint8_t ch, attHandleValueNoti_t *pNoti)
{
    if (!(Ecs_GetCccd(connHandle, ch) & GATT_CLIENT_CFG_NOTIFY))
        return bleIncorrectMode;
    pNoti->handle = ecs_value_handle(ch);
    return GATT_Notification(connHandle, pNoti, FALSE);
}

static bStatus_t ecs_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                uint16_t *pLen, uint16_t offset, uint16_t maxLen,
                                uint8_t method)
{
    (void)connHandle;
    (void)method;

    /* GATTServApp answers CCCD reads itself; only Status is readable here. */
    if (pAttr->handle == ecsAttrTbl[IDX_STATUS_VALUE].handle) {
        if (offset >= LINK_STATUS_LEN)
            return ATT_ERR_INVALID_OFFSET;
        *pLen = MIN(maxLen, (uint16_t)(LINK_STATUS_LEN - offset));
        tmos_memcpy(pValue, &ecsStatus[offset], *pLen);
        return SUCCESS;
    }
    *pLen = 0;
    return ATT_ERR_ATTR_NOT_FOUND;
}

static bStatus_t ecs_WriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                 uint16_t len, uint16_t offset, uint8_t method)
{
    bStatus_t status;
    (void)method;

    /* If attribute permissions require authorization to write, return error */
    if (gattPermitAuthorWrite(pAttr->permissions))
        return ATT_ERR_INSUFFICIENT_AUTHOR;

    if (pAttr->type.len == ATT_BT_UUID_SIZE) {
        uint16_t uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);
        if (uuid != GATT_CLIENT_CHAR_CFG_UUID)
            return ATT_ERR_ATTR_NOT_FOUND;
        status = GATTServApp_ProcessCCCWriteReq(connHandle, pAttr, pValue, len, offset,
                                                GATT_CLIENT_CFG_NOTIFY);
        if (status == SUCCESS && ecsCccdCB)
            ecsCccdCB();
        return status;
    }

    if (pAttr->handle == ecsAttrTbl[IDX_CONTROL_VALUE].handle) {
        if (offset != 0)
            return ATT_ERR_ATTR_NOT_LONG;
        if (len < 1u || len > LINK_CMD_MAX_LEN)
            return ATT_ERR_INVALID_VALUE_SIZE;
        tmos_memcpy(ecsControl, pValue, len);
        if (ecsControlCB)
            ecsControlCB(connHandle, ecsControl, len);
        return SUCCESS;
    }
    return ATT_ERR_ATTR_NOT_FOUND;
}

/* Reset Client Char Config when the connection has dropped. Called by the
 * application on GAP_LINK_TERMINATED_EVENT (HeartRate example style). */
void Ecs_Disconnected(uint16_t connHandle)
{
    /* Make sure this is not loopback connection */
    if (connHandle != LOOPBACK_CONNHANDLE) {
        GATTServApp_InitCharCfg(connHandle, ecsEcgCccd);
        GATTServApp_InitCharCfg(connHandle, ecsPpgCccd);
        GATTServApp_InitCharCfg(connHandle, ecsEventsCccd);
        GATTServApp_InitCharCfg(connHandle, ecsStatusCccd);
    }
}
