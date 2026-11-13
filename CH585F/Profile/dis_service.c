/* Device Information service (0x180A). Structure and read callback follow
 * WCH's Profile/devinfoservice.c; only the five strings of link_fmt.h. */
#include "std_services.h"

#include "app_config.h"

static const uint8_t disServUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(DEVINFO_SERV_UUID),
                                                      HI_UINT16(DEVINFO_SERV_UUID)};
static const uint8_t disMfrUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(MANUFACTURER_NAME_UUID),
                                                     HI_UINT16(MANUFACTURER_NAME_UUID)};
static const uint8_t disModelUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(MODEL_NUMBER_UUID),
                                                       HI_UINT16(MODEL_NUMBER_UUID)};
static const uint8_t disSerialUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(SERIAL_NUMBER_UUID),
                                                        HI_UINT16(SERIAL_NUMBER_UUID)};
static const uint8_t disFwRevUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(FIRMWARE_REV_UUID),
                                                       HI_UINT16(FIRMWARE_REV_UUID)};
static const uint8_t disSwRevUUID[ATT_BT_UUID_SIZE] = {LO_UINT16(SOFTWARE_REV_UUID),
                                                       HI_UINT16(SOFTWARE_REV_UUID)};

static const gattAttrType_t disService = {ATT_BT_UUID_SIZE, disServUUID};

static uint8_t disReadProps = GATT_PROP_READ;

static const uint8_t disMfr[] = APP_DIS_MANUFACTURER;
static const uint8_t disModel[] = APP_DIS_MODEL;
static const uint8_t disFwRev[] = APP_FW_STRING;
static uint8_t disSerial[DIS_STR_MAX];
static uint8_t disSerialLen;
static uint8_t disSwRev[DIS_STR_MAX] = "unknown";
static uint8_t disSwRevLen = 7;

static gattAttribute_t disAttrTbl[] = {
    {{ATT_BT_UUID_SIZE, primaryServiceUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&disService},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &disReadProps},
    {{ATT_BT_UUID_SIZE, disMfrUUID}, GATT_PERMIT_READ, 0, (uint8_t *)disMfr},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &disReadProps},
    {{ATT_BT_UUID_SIZE, disModelUUID}, GATT_PERMIT_READ, 0, (uint8_t *)disModel},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &disReadProps},
    {{ATT_BT_UUID_SIZE, disSerialUUID}, GATT_PERMIT_READ, 0, disSerial},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &disReadProps},
    {{ATT_BT_UUID_SIZE, disFwRevUUID}, GATT_PERMIT_READ, 0, (uint8_t *)disFwRev},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &disReadProps},
    {{ATT_BT_UUID_SIZE, disSwRevUUID}, GATT_PERMIT_READ, 0, disSwRev},
};

static bStatus_t dis_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                uint16_t *pLen, uint16_t offset, uint16_t maxLen,
                                uint8_t method);

static gattServiceCBs_t disCBs = {
    dis_ReadAttrCB, /* Read callback function pointer */
    NULL,           /* Write callback function pointer */
    NULL            /* Authorization callback function pointer */
};

bStatus_t Dis_AddService(void)
{
    return GATTServApp_RegisterService(disAttrTbl, GATT_NUM_ATTRS(disAttrTbl),
                                       GATT_MAX_ENCRYPT_KEY_SIZE, &disCBs);
}

static uint8_t dis_copy(uint8_t *dst, const uint8_t *src, uint16_t len)
{
    if (len > DIS_STR_MAX)
        len = DIS_STR_MAX;
    tmos_memcpy(dst, src, len);
    return (uint8_t)len;
}

void Dis_SetSerial(const char *s)
{
    disSerialLen = dis_copy(disSerial, (const uint8_t *)s, (uint16_t)strlen(s));
}

void Dis_SetSoftwareRev(const uint8_t *p, uint8_t len)
{
    disSwRevLen = dis_copy(disSwRev, p, len);
}

static bStatus_t dis_read_str(const uint8_t *s, uint16_t len, uint8_t *pValue,
                              uint16_t *pLen, uint16_t offset, uint16_t maxLen)
{
    /* verify offset; an empty string reads as zero bytes */
    if (offset > len || (len != 0u && offset == len))
        return ATT_ERR_INVALID_OFFSET;
    *pLen = MIN(maxLen, (uint16_t)(len - offset));
    tmos_memcpy(pValue, &s[offset], *pLen);
    return SUCCESS;
}

static bStatus_t dis_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                uint16_t *pLen, uint16_t offset, uint16_t maxLen,
                                uint8_t method)
{
    uint16_t uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);
    (void)connHandle;
    (void)method;

    switch (uuid) {
    case MANUFACTURER_NAME_UUID:
        return dis_read_str(disMfr, sizeof disMfr - 1u, pValue, pLen, offset, maxLen);
    case MODEL_NUMBER_UUID:
        return dis_read_str(disModel, sizeof disModel - 1u, pValue, pLen, offset, maxLen);
    case SERIAL_NUMBER_UUID:
        return dis_read_str(disSerial, disSerialLen, pValue, pLen, offset, maxLen);
    case FIRMWARE_REV_UUID:
        return dis_read_str(disFwRev, sizeof disFwRev - 1u, pValue, pLen, offset, maxLen);
    case SOFTWARE_REV_UUID:
        return dis_read_str(disSwRev, disSwRevLen, pValue, pLen, offset, maxLen);
    default:
        *pLen = 0;
        return ATT_ERR_ATTR_NOT_FOUND;
    }
}
