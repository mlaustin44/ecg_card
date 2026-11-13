/* BLE peripheral TMOS task. GAP role set-up, bond manager, connection
 * parameter update and notification sending follow WCH's Peripheral
 * example (APP/peripheral.c); the fast/slow advertising schedule follows
 * the HeartRate example (heartrate.c, HeartRateGapStateCB); the
 * peripheral-initiated MTU exchange follows HID_BTSSP_NFC (hidconsumer.c).
 *
 * Every notification goes through one bounded queue (rl_queue.c): it is
 * filled from MCU frames and drained here in bursts. When the stack has no
 * buffer the pump backs off for APP_NOTIFY_BACKOFF instead of retrying;
 * when the queue is full the oldest entry is dropped and counted. */
#include <stdbool.h>

#include "CONFIG.h"

#include "app_config.h"
#include "ble_app.h"
#include "ecg_service.h"
#include "link_mgr.h"
#include "rl_ble_fmt.h"
#include "rl_queue.h"
#include "std_services.h"

/* Task events */
#define BA_EVT_START_DEVICE 0x0001
#define BA_EVT_PARAM_UPDATE 0x0002
#define BA_EVT_NOTIFY       0x0004

/* Queue entry kinds: ECS_CHAR_* (0..3), then these */
#define Q_HR   4u
#define Q_BATT 5u

/* Advertising phase */
#define ADV_OFF  0u
#define ADV_FAST 1u
#define ADV_SLOW 2u

static tmosTaskID s_task = INVALID_TASK_ID;
static gapRole_States_t s_role_state = GAPROLE_INIT;

static uint16_t s_conn = GAP_CONNHANDLE_INIT;
static uint16_t s_interval, s_latency, s_timeout;
static uint16_t s_mtu = ATT_MTU_SIZE;
static uint8_t s_phy;
static uint8_t s_subscribed;

static uint8_t s_adv_phase = ADV_OFF;
static bool s_adv_wanted; /* MCU asked for advertising (LINK_RC_ADV_START) */
static bool s_adv_cancel; /* advertising stopped on purpose */
static bool s_shutdown;
static bool s_backoff;    /* waiting for stack buffers */

static rl_queue_t s_q;
static rl_hr_sel_t s_hr;
static uint32_t s_unsub;    /* frames/entries for an unsubscribed characteristic */
static uint32_t s_busy;     /* back-offs: no stack buffer */
static uint32_t s_mtu_big;  /* entries longer than MTU - 3 (also in dropped) */
static uint32_t s_noti_err; /* other GATT_Notification errors (also in dropped) */
static uint8_t s_last_err;
static uint32_t s_last_stats_ms;

/* GAP data (static: the role keeps the pointers) */
static uint8_t s_adv_data[RL_ADV_MAX];
static uint8_t s_scan_rsp[RL_ADV_MAX];
static uint8_t s_dev_name[GAP_DEVICE_NAME_LEN];
static uint8_t s_boot_addr[B_ADDR_LEN]; /* GetMACAddress() at boot (LINK_HELLO) */

static tmosEvents ba_process_event(tmosTaskID task_id, tmosEvents events);
static void ba_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent);
static void ba_param_update_cb(uint16_t connHandle, uint16_t connInterval,
                               uint16_t connSlaveLatency, uint16_t connTimeout);
static void ba_cccd_changed(void);
static void ba_control_write(uint16_t connHandle, const uint8_t *pValue, uint16_t len);

/* GAP Role Callbacks */
static gapRolesCBs_t s_role_cbs = {
    ba_state_cb,       /* Profile State Change Callbacks */
    NULL,              /* RSSI */
    ba_param_update_cb /* Connection parameter update */
};

/* Broadcast Callbacks */
static gapRolesBroadcasterCBs_t s_bcast_cbs = {
    NULL, /* Not used in peripheral role */
    NULL  /* Receive scan request callback */
};

/* GAP Bond Manager Callbacks (Just Works: no passcode) */
static gapBondCBs_t s_bond_cbs = {
    NULL, /* Passcode callback */
    NULL, /* Pairing / Bonding state Callback */
    NULL  /* oob callback */
};

static bool connected(void)
{
    return s_conn != GAP_CONNHANDLE_INIT;
}

void ble_app_init(const uint8_t addr[6])
{
    char name[RL_NAME_LEN + 1u];
    char serial[RL_SERIAL_LEN + 1u];
    uint8_t adv_len, scan_len;

    s_task = TMOS_ProcessEventRegister(ba_process_event);
    rl_queue_init(&s_q);
    tmos_memcpy(s_boot_addr, addr, B_ADDR_LEN);

    rl_ble_name(addr, name);
    rl_ble_serial(addr, serial);
    adv_len = (uint8_t)rl_ble_adv_data(s_adv_data);
    scan_len = (uint8_t)rl_ble_scan_rsp(name, APP_CONN_INTERVAL, APP_CONN_INTERVAL,
                                        APP_ADV_TX_POWER_DBM, s_scan_rsp);

    /* Setup the GAP Peripheral Role Profile. Advertising starts only on
     * LINK_RC_ADV_START from the MCU. */
    {
        uint8_t initial_advertising_enable = FALSE;
        uint16_t desired_min_interval = APP_CONN_INTERVAL;
        uint16_t desired_max_interval = APP_CONN_INTERVAL;

        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
        GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, scan_len, s_scan_rsp);
        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, adv_len, s_adv_data);
        GAPRole_SetParameter(GAPROLE_MIN_CONN_INTERVAL, sizeof(uint16_t), &desired_min_interval);
        GAPRole_SetParameter(GAPROLE_MAX_CONN_INTERVAL, sizeof(uint16_t), &desired_max_interval);
    }

    /* Advertising interval and fast-phase duration */
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, APP_ADV_FAST_INT);
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, APP_ADV_FAST_INT);
    GAP_SetParamValue(TGAP_GEN_DISC_ADV_MIN, APP_ADV_FAST_DURATION);

    /* Setup the GAP Bond Manager: Just Works bonding (no MITM, no IO) */
    {
        uint32_t passkey = 0;
        uint8_t pairMode = GAPBOND_PAIRING_MODE_WAIT_FOR_REQ;
        uint8_t mitm = FALSE;
        uint8_t bonding = TRUE;
        uint8_t ioCap = GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT;
        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    /* Initialize GATT attributes */
    GGS_AddService(GATT_ALL_SERVICES);         /* GAP */
    GATTServApp_AddService(GATT_ALL_SERVICES); /* GATT attributes */
    Dis_AddService();
    Bas_AddService(ba_cccd_changed);
    Hrs_AddService(ba_cccd_changed);
    Ecs_AddService(ba_cccd_changed, ba_control_write);

    {
        uint8_t status[LINK_STATUS_LEN];
        rl_msg_status_default(status);
        Ecs_SetStatus(status);
    }
    Bas_SetLevel(0); /* unknown until the first LINK_STATUS */
    Dis_SetSerial(serial);

    /* Set the GAP Characteristics */
    tmos_memset(s_dev_name, 0, sizeof s_dev_name);
    tmos_memcpy(s_dev_name, name, RL_NAME_LEN);
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, s_dev_name);

#if APP_MTU_EXCHANGE
    GATT_InitClient(); /* needed for GATT_ExchangeMTU (HID_BTSSP_NFC example) */
#endif

    /* Register receive scan request callback */
    GAPRole_BroadcasterSetCB(&s_bcast_cbs);

    /* Setup a delayed profile startup */
    tmos_set_event(s_task, BA_EVT_START_DEVICE);
}

/* ---- advertising -------------------------------------------------------- */

static void ba_set_adv_params(uint8_t phase)
{
    uint16_t intv = phase == ADV_SLOW ? APP_ADV_SLOW_INT : APP_ADV_FAST_INT;
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, intv);
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, intv);
    /* 0 = advertise until stopped */
    GAP_SetParamValue(TGAP_GEN_DISC_ADV_MIN, phase == ADV_SLOW ? 0 : APP_ADV_FAST_DURATION);
}

static void ba_adv_enable(uint8_t phase)
{
    uint8_t advState = TRUE;
    ba_set_adv_params(phase);
    s_adv_phase = phase;
    s_adv_cancel = FALSE;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advState);
    link_conn_state_changed();
}

static void ba_adv_disable(void)
{
    uint8_t advState = FALSE;
    if (s_adv_phase != ADV_OFF)
        s_adv_cancel = TRUE;
    s_adv_phase = ADV_OFF;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advState);
    link_conn_state_changed();
}

/* Advertising ended by itself (fast phase timed out) or because we
 * stopped it. */
static void ba_adv_ended(void)
{
    if (s_adv_cancel || !s_adv_wanted || s_shutdown || connected()) {
        s_adv_cancel = FALSE;
        if (!connected())
            s_adv_phase = ADV_OFF;
        link_conn_state_changed();
        return;
    }
    /* fast phase over: switch to slow */
    ba_adv_enable(ADV_SLOW);
    link_log("ble: slow advertising");
}

uint8_t ble_app_adv_start(void)
{
    if (s_shutdown || connected())
        return LINK_RES_BUSY;
    s_adv_wanted = TRUE;
    if (s_adv_phase == ADV_OFF)
        ba_adv_enable(ADV_FAST);
    return LINK_RES_OK;
}

uint8_t ble_app_adv_stop(void)
{
    s_adv_wanted = FALSE;
    ba_adv_disable();
    return LINK_RES_OK;
}

uint8_t ble_app_disconnect(void)
{
    if (connected())
        GAPRole_TerminateLink(s_conn);
    return LINK_RES_OK;
}

void ble_app_shutdown(void)
{
    s_shutdown = TRUE;
    s_adv_wanted = FALSE;
    ba_adv_disable();
    if (connected())
        GAPRole_TerminateLink(s_conn);
    rl_queue_flush(&s_q);
}

void ble_app_set_sw_rev(const uint8_t *text, size_t len)
{
    Dis_SetSoftwareRev(text, (uint8_t)(len > DIS_STR_MAX ? DIS_STR_MAX : len));
}

/* ---- subscriptions and connection state ------------------------------- */

static uint8_t ba_subscriptions(void)
{
    uint8_t m = 0;
    if (!connected())
        return 0;
    if (Ecs_GetCccd(s_conn, ECS_CHAR_ECG) & GATT_CLIENT_CFG_NOTIFY)
        m |= LINK_SUB_ECG;
    if (Ecs_GetCccd(s_conn, ECS_CHAR_PPG) & GATT_CLIENT_CFG_NOTIFY)
        m |= LINK_SUB_PPG;
    if (Ecs_GetCccd(s_conn, ECS_CHAR_EVENTS) & GATT_CLIENT_CFG_NOTIFY)
        m |= LINK_SUB_EVENTS;
    if (Ecs_GetCccd(s_conn, ECS_CHAR_STATUS) & GATT_CLIENT_CFG_NOTIFY)
        m |= LINK_SUB_STATUS;
    if (Hrs_GetCccd(s_conn) & GATT_CLIENT_CFG_NOTIFY)
        m |= LINK_SUB_HR;
    return m;
}

static void ba_cccd_changed(void)
{
    uint8_t m = ba_subscriptions();
    if (m != s_subscribed) {
        s_subscribed = m;
        link_conn_state_changed();
    }
}

void ble_app_conn_state(rl_conn_state_t *cs)
{
    if (connected())
        cs->state = LINK_CS_CONNECTED;
    else if (s_adv_phase == ADV_FAST)
        cs->state = LINK_CS_ADV_FAST;
    else if (s_adv_phase == ADV_SLOW)
        cs->state = LINK_CS_ADV_SLOW;
    else
        cs->state = LINK_CS_IDLE;
    cs->mtu = connected() ? s_mtu : 0u;
    cs->interval = connected() ? s_interval : 0u;
    cs->subscribed = s_subscribed;
    cs->dropped = rl_queue_dropped16(&s_q);
    cs->phy = connected() ? s_phy : 0u;
}

/* ---- notifications -------------------------------------------------------- */

static void ba_backoff(void)
{
    s_backoff = TRUE;
    s_busy++;
    tmos_start_task(s_task, BA_EVT_NOTIFY, APP_NOTIFY_BACKOFF);
}

static void ba_enqueue(uint8_t kind, const uint8_t *p, size_t n)
{
    rl_queue_push(&s_q, kind, p, n);
    if (!s_backoff)
        tmos_set_event(s_task, BA_EVT_NOTIFY);
}

static bStatus_t ba_notify(uint8_t kind, attHandleValueNoti_t *noti)
{
    switch (kind) {
    case Q_HR:
        return Hrs_Notify(s_conn, noti);
    case Q_BATT:
        return Bas_Notify(s_conn, noti);
    default:
        return Ecs_Notify(s_conn, kind, noti);
    }
}

static void ba_pump(void)
{
    unsigned i;

    s_backoff = FALSE;
    for (i = 0; i < APP_NOTIFY_BURST; i++) {
        const rl_qent_t *e = rl_queue_peek(&s_q);
        attHandleValueNoti_t noti;
        bStatus_t st;

        if (e == NULL)
            return;
        if (!connected()) {
            rl_queue_flush(&s_q);
            return;
        }
        if ((uint16_t)(e->len + 3u) > ATT_GetMTU(s_conn)) {
            s_mtu_big++;
            rl_queue_drop_head(&s_q);
            continue;
        }
        noti.len = e->len;
        noti.pValue = GATT_bm_alloc(s_conn, ATT_HANDLE_VALUE_NOTI, noti.len, NULL, 0);
        if (noti.pValue == NULL) {
            ba_backoff();
            return;
        }
        tmos_memcpy(noti.pValue, e->data, e->len);
        st = ba_notify(e->kind, &noti);
        if (st == SUCCESS) {
            rl_queue_pop(&s_q);
            continue;
        }
        GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
        if (st == MSG_BUFFER_NOT_AVAIL || st == bleMemAllocError || st == blePending) {
            ba_backoff();
            return;
        }
        if (st == bleIncorrectMode) { /* unsubscribed meanwhile */
            s_unsub++;
            rl_queue_discard_head(&s_q);
            continue;
        }
        s_noti_err++;
        s_last_err = st;
        rl_queue_drop_head(&s_q);
    }
    /* More queued: yield to the other tasks, then continue. */
    if (rl_queue_count(&s_q))
        tmos_set_event(s_task, BA_EVT_NOTIFY);
}

/* ---- data from the MCU -------------------------------------------------- */

static void ba_hr_from_event(const uint8_t *p, size_t n)
{
    uint8_t kind, v[2];
    uint16_t bpm;

    if (!rl_msg_event_bpm(p, n, &kind, &bpm))
        return;
    if (!rl_hr_select(&s_hr, kind, link_now_ms()) || bpm == 0u)
        return;
    if (!connected() || !(Hrs_GetCccd(s_conn) & GATT_CLIENT_CFG_NOTIFY))
        return;
    rl_msg_hr_measurement(bpm, v);
    ba_enqueue(Q_HR, v, sizeof v);
}

void ble_app_stream(uint8_t type, const uint8_t *p, size_t n)
{
    uint8_t ch;

    if (type == STREAM_EVENT)
        ba_hr_from_event(p, n);
    ch = type == STREAM_ECG_BLOCK ? ECS_CHAR_ECG
       : type == STREAM_PPG_BLOCK ? ECS_CHAR_PPG
                                  : ECS_CHAR_EVENTS;
    if (s_shutdown || !connected() || !(Ecs_GetCccd(s_conn, ch) & GATT_CLIENT_CFG_NOTIFY)) {
        s_unsub++;
        return;
    }
    ba_enqueue(ch, p, n);
}

void ble_app_status(const rl_status_t *st)
{
    uint8_t level = rl_msg_battery_level(st->batt_pct);

    Ecs_SetStatus(st->raw);
    if (!s_shutdown && connected() &&
        (Ecs_GetCccd(s_conn, ECS_CHAR_STATUS) & GATT_CLIENT_CFG_NOTIFY))
        ba_enqueue(ECS_CHAR_STATUS, st->raw, LINK_STATUS_LEN);
    if (level != Bas_GetLevel()) {
        Bas_SetLevel(level);
        if (!s_shutdown && connected() && (Bas_GetCccd(s_conn) & GATT_CLIENT_CFG_NOTIFY))
            ba_enqueue(Q_BATT, &level, 1);
    }
}

static void ba_control_write(uint16_t connHandle, const uint8_t *pValue, uint16_t len)
{
    (void)connHandle;
    if (!s_shutdown)
        link_send_cmd(pValue, len);
}

void ble_app_tick(uint32_t now_ms)
{
    ba_cccd_changed();
    if (connected() && (uint32_t)(now_ms - s_last_stats_ms) >= APP_STATS_PERIOD_MS) {
        s_last_stats_ms = now_ms;
        link_log("ble: sent %u drop %u q %u busy %u unsub %u big %u err %u/0x%02X",
                 (unsigned)s_q.sent, (unsigned)s_q.dropped, rl_queue_count(&s_q),
                 (unsigned)s_busy, (unsigned)s_unsub, (unsigned)s_mtu_big,
                 (unsigned)s_noti_err, s_last_err);
    }
}

/* ---- GAP / GATT events ---------------------------------------------------- */

static void ba_link_established(gapRoleEvent_t *pEvent)
{
    gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;

    /* See if already connected */
    if (connected()) {
        GAPRole_TerminateLink(event->connectionHandle);
        return;
    }
    s_conn = event->connectionHandle;
    s_interval = event->connInterval;
    s_latency = event->connLatency;
    s_timeout = event->connTimeout;
    s_mtu = ATT_MTU_SIZE;
    s_phy = GAP_PHY_VAL_LE_1M;
    s_adv_phase = ADV_OFF;
    s_adv_cancel = FALSE;
    /* if the role resumes advertising after a link loss, start fast */
    ba_set_adv_params(ADV_FAST);
    rl_queue_flush(&s_q);

    if (s_shutdown) {
        GAPRole_TerminateLink(s_conn);
        return;
    }

    /* Set timer for param update event */
    tmos_start_task(s_task, BA_EVT_PARAM_UPDATE, APP_PARAM_UPDATE_DELAY);

#if APP_MTU_EXCHANGE
    {
        attExchangeMTUReq_t req;
        req.clientRxMTU = BLE_BUFF_MAX_LEN - 4; /* 247 */
        GATT_ExchangeMTU(s_conn, &req, s_task);
    }
#endif

    link_log("ble: connected %02X:%02X:%02X:%02X:%02X:%02X int %u lat %u to %u",
             event->devAddr[5], event->devAddr[4], event->devAddr[3], event->devAddr[2],
             event->devAddr[1], event->devAddr[0], s_interval, s_latency, s_timeout);
    s_last_stats_ms = link_now_ms();
    s_subscribed = ba_subscriptions();
    link_conn_state_changed();
}

static void ba_link_terminated(gapRoleEvent_t *pEvent)
{
    gapTerminateLinkEvent_t *event = (gapTerminateLinkEvent_t *)pEvent;
    unsigned lost;

    if (event->connectionHandle != s_conn)
        return;
    Ecs_Disconnected(s_conn);
    Std_Disconnected(s_conn);
    s_conn = GAP_CONNHANDLE_INIT;
    s_interval = s_latency = s_timeout = 0;
    s_mtu = ATT_MTU_SIZE;
    s_phy = 0;
    s_subscribed = 0;
    tmos_stop_task(s_task, BA_EVT_PARAM_UPDATE);
    lost = rl_queue_flush(&s_q);

    if (!s_shutdown)
        link_log("ble: disconnected reason 0x%02X, %u queued dropped", event->reason, lost);

    /* link loss: fast advertising again if the MCU still wants it
     * (HeartRate example); otherwise make sure the role does not resume */
    if (s_adv_wanted && !s_shutdown)
        ba_adv_enable(ADV_FAST);
    else
        ba_adv_disable();
}

static void ba_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    switch (newState & GAPROLE_STATE_ADV_MASK) {
    case GAPROLE_STARTED: {
        /* The stack's own address, to check against the one in LINK_HELLO
         * and the advertised name (byte order of GetMACAddress()). */
        uint8_t a[B_ADDR_LEN];
        GAPRole_GetParameter(GAPROLE_BD_ADDR, a);
        link_log("ble: started, bd_addr %02X:%02X:%02X:%02X:%02X:%02X %s HELLO", a[5], a[4],
                 a[3], a[2], a[1], a[0],
                 tmos_memcmp(a, s_boot_addr, B_ADDR_LEN) ? "matches" : "DIFFERS FROM");
        break;
    }

    case GAPROLE_ADVERTISING:
        if (pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            ba_link_terminated(pEvent);
        break;

    case GAPROLE_CONNECTED:
        if (pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            ba_link_established(pEvent);
        break;

    case GAPROLE_WAITING:
        if (pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT) {
            ba_link_terminated(pEvent);
        } else if (pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT) {
            /* connection attempt failed: resume the current phase */
            if (s_adv_wanted && !s_shutdown && !connected())
                ba_adv_enable(s_adv_phase == ADV_SLOW ? ADV_SLOW : ADV_FAST);
        } else if ((s_role_state & GAPROLE_STATE_ADV_MASK) == GAPROLE_ADVERTISING) {
            ba_adv_ended(); /* fast phase timed out, or stopped by us */
        }
        break;

    case GAPROLE_ERROR:
        link_log("ble: role error, opcode 0x%02X", pEvent->gap.opcode);
        break;

    default:
        break;
    }
    s_role_state = newState;
}

static void ba_param_update_cb(uint16_t connHandle, uint16_t connInterval,
                               uint16_t connSlaveLatency, uint16_t connTimeout)
{
    if (connHandle != s_conn)
        return;
    s_interval = connInterval;
    s_latency = connSlaveLatency;
    s_timeout = connTimeout;
    link_log("ble: params int %u lat %u to %u", connInterval, connSlaveLatency, connTimeout);
    link_conn_state_changed();
}

static void ba_process_gap_msg(gapRoleEvent_t *pEvent)
{
    if (pEvent->gap.opcode == GAP_PHY_UPDATE_EVENT &&
        pEvent->linkPhyUpdate.connectionHandle == s_conn) {
        uint8_t phy = pEvent->linkPhyUpdate.connTxPHYS;
        s_phy = (phy == GAP_PHY_VAL_LE_1M || phy == GAP_PHY_VAL_LE_2M) ? phy : 0u;
        link_log("ble: phy tx %u rx %u", pEvent->linkPhyUpdate.connTxPHYS,
                 pEvent->linkPhyUpdate.connRxPHYS);
        link_conn_state_changed();
    }
}

static void ba_process_gatt_msg(gattMsgEvent_t *pMsg)
{
    if (connected() && pMsg->connHandle == s_conn) {
        if (pMsg->method == ATT_MTU_UPDATED_EVENT) {
            s_mtu = ATT_GetMTU(s_conn);
            link_log("ble: mtu %u (event %u)", s_mtu, pMsg->msg.mtuEvt.MTU);
            link_conn_state_changed();
        } else if (pMsg->method == ATT_ERROR_RSP &&
                   pMsg->msg.errorRsp.reqOpcode == ATT_EXCHANGE_MTU_REQ) {
            link_log("ble: mtu exchange error 0x%02X", pMsg->msg.errorRsp.errCode);
        }
    }
    GATT_bm_free(&pMsg->msg, pMsg->method);
}

static void ba_process_msg(tmos_event_hdr_t *pMsg)
{
    switch (pMsg->event) {
    case GAP_MSG_EVENT:
        ba_process_gap_msg((gapRoleEvent_t *)pMsg);
        break;
    case GATT_MSG_EVENT:
        ba_process_gatt_msg((gattMsgEvent_t *)pMsg);
        break;
    default:
        break;
    }
}

static tmosEvents ba_process_event(tmosTaskID task_id, tmosEvents events)
{
    (void)task_id;

    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg;
        if ((pMsg = tmos_msg_receive(s_task)) != NULL) {
            ba_process_msg((tmos_event_hdr_t *)pMsg);
            /* Release the TMOS message */
            tmos_msg_deallocate(pMsg);
        }
        /* return unprocessed events */
        return (events ^ SYS_EVENT_MSG);
    }

    if (events & BA_EVT_START_DEVICE) {
        /* Start the Device */
        GAPRole_PeripheralStartDevice(s_task, &s_bond_cbs, &s_role_cbs);
        return (events ^ BA_EVT_START_DEVICE);
    }

    if (events & BA_EVT_PARAM_UPDATE) {
        /* Send connect param update request. Returns 0x18 (InvalidRange)
         * when the current parameters already meet the request. */
        if (connected()) {
            bStatus_t st = GAPRole_PeripheralConnParamUpdateReq(
                s_conn, APP_CONN_INTERVAL, APP_CONN_INTERVAL, APP_CONN_LATENCY,
                APP_CONN_TIMEOUT, s_task);
            if (st != SUCCESS)
                link_log("ble: param update request 0x%02X", st);
        }
        return (events ^ BA_EVT_PARAM_UPDATE);
    }

    if (events & BA_EVT_NOTIFY) {
        ba_pump();
        return (events ^ BA_EVT_NOTIFY);
    }

    /* Discard unknown events */
    return 0;
}
