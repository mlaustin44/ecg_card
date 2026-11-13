/* hal.h acquisition: ECG (TIMG6 -> ADC0 -> DMA ring), PPG (TIMA0 -> LED_BLANK
 * and ADC1 -> ISR frames), PPG on-chip analog, one-shot conversions.
 *
 * ---- Time base ------------------------------------------------------------
 * TIMA0 (PPG) and TIMG6 (ECG) are both PD1 timers clocked from BUSCLK = MCLK,
 * prescaled to one common 4 MHz tick (80 MHz / 20 or 32 MHz / 8), counting
 * down with LOAD = period - 1 and auto-reload.
 *
 * Start: both timers have CCCTL0.LCOND = "trigger rising edge", CTRCTL.CLC =
 * CCCTL0 and TSEL = TRIG0 with TE = 1. TRIG0 is the cross-trigger output of
 * TIMA0 (TI SysConfig mapping for MSPM0G1x0x/G3x0x: TIMA0 = TRIG0 in PD1).
 * hal_acq_start() writes TIMA0.CTTRIG once; the same edge loads both counters
 * with LOAD and sets their EN bits (CTRCTL.EN "may be set as the result of an
 * LCOND condition"), exactly as TI's timx_timer_mode_pwm_x_trig_stop_restore
 * example starts TIMA1 + TIMG7.
 *
 * With t = ticks after that edge, a down counter holds (P - 1 - t). The hal.h
 * timeline origin is defined as T0 = edge + 1 tick, so an event at timeline
 * tick T fires when the counter equals P - 2 - T (CC_AT below):
 *   ECG:  P = 1000, CC0 = 998          -> sample n at T = n * 250 us
 *   PPG:  P = 40000, CC0 = 1198 (dark) -> T = k*10 ms + 9700 us
 *                    CC3 =  798 (LED_BLANK low, CDACT) -> 9800 us
 *                    CC1 =  198 (on)   -> T = k*10 ms + 9950 us
 *         LED_BLANK goes high on the reload (LACT) at T = k*10 ms + 9999.75 us.
 * Residual fixed offsets (documented, not corrected):
 *   - each timer has its own prescaler counter; if the cross trigger does
 *     not reset it, the two time bases differ by a constant < 1 tick
 *     (< 250 ns) for the whole session;
 *   - conversion latency: ADC0 and ADC1 both start sampling a few ADCCLK
 *     cycles after their event; with 4x averaging and 2 us sampling the
 *     effective sample instant is ~+5 us for ECG and for TIA_OUT (first in
 *     the ADC1 sequence) and ~+15 us for PPG_HI. ECG vs TIA_OUT therefore
 *     share the same latency.
 *
 * ---- ECG ring (never stops) -----------------------------------------------
 * ADC0: repeat-single-channel, event-triggered, MEMCTL0.TRIG = TRIGGER_NEXT
 * (each conversion needs a new TIMG6 event), HW average x4, 2 us sampling.
 * Every MEM0 result raises the ADC0 DMA trigger; DMA channel 0 (a FULL
 * channel) runs in REPEAT-SINGLE mode MEMRES0 -> ring[512]: after the 512th
 * transfer DMASA/DMADA/DMASZ reload automatically and the channel stays
 * enabled, so the ring is circular in hardware.
 * The one software duty: the ADC clears its own CTL2.DMAEN when it sees the
 * DMA "done" at each lap end (hw_adc12.h: "Software has to re-enable DMAEN").
 * The ADC0 DMA_DONE interrupt (priority 0) re-enables it; the deadline is the
 * next conversion, 250 us later, once per 128 ms. A sample that lands while
 * DMAEN is off is detected (MEMRESIFG0 still set) and counted as lost.
 * The ISR also counts laps; the reader derives the total number of samples
 * written as laps * 512 + (DMADA - ring) / 2 (see ecg_written()).
 *
 * ---- PPG frames (resynchronised every frame) -------------------------------
 * ADC1: one non-repeating 2-entry sequence per TIMA0 event (MEM0 = TIA_OUT,
 * MEM1 = PPG_HI, x4 averaging). The MEM1 interrupt reads both results, re-arms
 * ENC and classifies the pair by the TIMA0 counter value at that moment:
 * between the on and dark compare values -> dark, otherwise -> on. Classification
 * never depends on alternation, so a missed trigger costs one frame (counted
 * as an overrun) and the next frame is correct. */
#include <string.h>

#include "bsp_internal.h"

/* ---- Channels ------------------------------------------------------------ */
#define ECG_ADC_CHAN   DL_ADC12_INPUT_CHAN_12 /* PA14 = A0_12 */
#define TIA_ADC_CHAN   DL_ADC12_INPUT_CHAN_8  /* PA22 = A1_8 (brief; see README) */
#define PPGHI_ADC_CHAN DL_ADC12_INPUT_CHAN_1  /* PA16 = A1_1 */
#define VMID_ADC_CHAN  DL_ADC12_INPUT_CHAN_4  /* PB17 = A1_4 */

/* ---- Time base ----------------------------------------------------------- */
#define TICK_HZ           4000000u
#define TICKS_PER_US      (TICK_HZ / 1000000u)
#define ECG_PERIOD_TICKS  (TICK_HZ / ECG_RAW_HZ)
#define PPG_PERIOD_TICKS  (PPG_FRAME_US * TICKS_PER_US)
#define CC_AT(P, T)       ((P) - 2u - (T))
#define ECG_CC            CC_AT(ECG_PERIOD_TICKS, 0u)
#define PPG_CC_DARK       CC_AT(PPG_PERIOD_TICKS, PPG_T_DARK_US * TICKS_PER_US)
#define PPG_CC_LED        CC_AT(PPG_PERIOD_TICKS, PPG_T_LED_ON_US * TICKS_PER_US)
#define PPG_CC_ON         CC_AT(PPG_PERIOD_TICKS, PPG_T_ON_SAMPLE_US * TICKS_PER_US)

_Static_assert(PPG_PERIOD_TICKS <= 65536u, "TIMA0 is 16-bit");
_Static_assert(ECG_PERIOD_TICKS * ECG_RAW_HZ == TICK_HZ, "ECG period not integral");
_Static_assert(PPG_T_DARK_US < PPG_T_LED_ON_US &&
               PPG_T_LED_ON_US < PPG_T_ON_SAMPLE_US &&
               PPG_T_ON_SAMPLE_US < PPG_FRAME_US, "PPG frame order");
_Static_assert(BSP_PLL_HZ % TICK_HZ == 0u && BSP_SYSOSC_HZ % TICK_HZ == 0u,
               "tick must divide both MCLK settings");

/* ---- ADC timing ---------------------------------------------------------- */
#define ADC_CLK_HZ        32000000u /* SYSOSC / 1, independent of MCLK */
#define ADC_SAMP_STREAM   64u       /* 2.0 us */
#define ADC_SAMP_ONESHOT  320u      /* 10 us for the one-shot diagnostics */

static const DL_ADC12_ClockConfig k_adc_clk = {
    .clockSel = DL_ADC12_CLOCK_SYSOSC,
    .divideRatio = DL_ADC12_CLOCK_DIVIDE_1,
    .freqRange = DL_ADC12_CLOCK_FREQ_RANGE_24_TO_32,
};

/* ---- State --------------------------------------------------------------- */
#define ECG_RING_N   512u /* 128 ms at 4 kHz; power of two */
#define ECG_SAFETY   16u  /* samples kept away from the DMA write pointer */
#define PPG_RING_N   32u  /* 320 ms */
_Static_assert((ECG_RING_N & (ECG_RING_N - 1u)) == 0u, "power of two");
_Static_assert((ECG_RING_N - ECG_SAFETY) * 1000u / ECG_RAW_HZ >= 64u, ">= 64 ms");

static uint16_t s_ecg_ring[ECG_RING_N] __attribute__((aligned(4)));
static volatile uint32_t s_ecg_laps;
static volatile uint32_t s_ecg_overruns;
static uint32_t s_ecg_rd;     /* samples consumed or dropped (main only) */
static uint32_t s_ecg_last_w; /* last total seen by the reader */

static hal_ppg_frame_t s_ppg_ring[PPG_RING_N];
static volatile uint32_t s_ppg_wr;
static uint32_t s_ppg_rd;
static volatile uint32_t s_ppg_overruns;
static bool s_ppg_have_dark;
static uint16_t s_ppg_dark_tia, s_ppg_dark_hi;

static volatile unsigned s_running;
static uint16_t s_dac12_code = 2048u;
static bool s_analog_on;

/* ======================================================================== */
/* Timers                                                                   */
/* ======================================================================== */
static void timer_power_on(GPTIMER_Regs *t)
{
    DL_Timer_reset(t);
    DL_Timer_enablePower(t);
    bsp_delay_cycles(32u);
}

/* Common down-counting, trigger-started time base. */
static void timer_base(GPTIMER_Regs *t, uint32_t period, bool triggered)
{
    const DL_Timer_ClockConfig clk = {
        .clockSel = DL_TIMER_CLOCK_BUSCLK,
        .divideRatio = DL_TIMER_CLOCK_DIVIDE_1,
        .prescale = (uint8_t) (g_mclk_hz / TICK_HZ - 1u),
    };
    DL_Timer_setClockConfig(t, &clk);
    DL_Timer_enableClock(t);
    DL_Timer_setCoreHaltBehavior(t, DL_TIMER_CORE_HALT_IMMEDIATE);
    DL_Timer_setLoadValue(t, period - 1u);
    DL_Timer_setCounterMode(t, DL_TIMER_COUNT_MODE_DOWN);
    DL_Timer_setCounterRepeatMode(t, DL_TIMER_REPEAT_MODE_ENABLED);
    DL_Timer_setCounterValueAfterEnable(t, DL_TIMER_COUNT_AFTER_EN_LOAD_VAL);
    /* CCCTL0 drives the counter: advance every TIMCLK, load on the rising
     * edge of "CCP or trigger". IFCTL0.ISEL = TRIG makes that edge the
     * selected external trigger (TSEL) rather than the CCP0 pin, exactly as
     * TI's cross-trigger example configures both timers. */
    DL_Timer_setCaptureCompareInput(t, DL_TIMER_CC_INPUT_INV_NOINVERT,
        DL_TIMER_CC_IN_SEL_TRIG, DL_TIMER_CC_0_INDEX);
    DL_Timer_setCaptureCompareCtl(t, DL_TIMER_CC_MODE_COMPARE,
        DL_TIMER_CC_ZCOND_NONE | DL_TIMER_CC_ACOND_TIMCLK |
            DL_TIMER_CC_LCOND_TRIG_RISE,
        DL_TIMER_CC_0_INDEX);
    DL_Timer_setCounterControl(t, DL_TIMER_CZC_CCCTL0_ZCOND,
        DL_TIMER_CAC_CCCTL0_ACOND, DL_TIMER_CLC_CCCTL0_LCOND);
    DL_Timer_setExternalTriggerEvent(t, DL_TIMER_EXT_TRIG_SEL_TRIG_0);
    if (triggered)
        DL_Timer_enableExternalTrigger(t);
    else
        DL_Timer_disableExternalTrigger(t);
}

static void tima0_config(bool ppg)
{
    timer_power_on(TIMA0);
    /* TIMA0 is the cross-trigger master and, like the master in TI's
     * cross-trigger example, is started by its own trigger. Without PPG it
     * just counts frames: no events, no CCP output. */
    timer_base(TIMA0, PPG_PERIOD_TICKS, true);
    if (ppg) {
        /* C0: dark ADC trigger (pin-less). */
        DL_Timer_setCaptureCompareValue(TIMA0, PPG_CC_DARK, DL_TIMER_CC_0_INDEX);
        /* C1: on ADC trigger (pin-less). */
        DL_Timer_setCaptureCompareCtl(TIMA0, DL_TIMER_CC_MODE_COMPARE,
            DL_TIMER_CC_ZCOND_NONE | DL_TIMER_CC_ACOND_TIMCLK, DL_TIMER_CC_1_INDEX);
        DL_Timer_setCaptureCompareValue(TIMA0, PPG_CC_ON, DL_TIMER_CC_1_INDEX);
        /* C3: LED_BLANK. High on reload, low at the compare (down count).
         * Initial value (while EN = 0) high. */
        DL_Timer_setCaptureCompareCtl(TIMA0, DL_TIMER_CC_MODE_COMPARE,
            DL_TIMER_CC_ZCOND_NONE | DL_TIMER_CC_ACOND_TIMCLK, DL_TIMER_CC_3_INDEX);
        DL_Timer_setCaptureCompareValue(TIMA0, PPG_CC_LED, DL_TIMER_CC_3_INDEX);
        DL_Timer_setCaptureCompareAction(TIMA0,
            DL_TIMER_CC_LACT_CCP_HIGH | DL_TIMER_CC_CDACT_CCP_LOW, DL_TIMER_CC_3_INDEX);
        DL_Timer_setCaptureCompareOutCtl(TIMA0, DL_TIMER_CC_OCTL_INIT_VAL_HIGH,
            DL_TIMER_CC_OCTL_INV_OUT_DISABLED, DL_TIMER_CC_OCTL_SRC_FUNCVAL,
            DL_TIMER_CC_3_INDEX);
        DL_Timer_setCCPDirection(TIMA0, DL_TIMER_CC3_OUTPUT);
        /* Both ADC triggers on publisher 0 -> event channel -> ADC1. */
        DL_Timer_enableEvent(TIMA0, DL_TIMER_EVENT_ROUTE_1,
            DL_TIMER_EVENT_CC0_DN_EVENT | DL_TIMER_EVENT_CC1_DN_EVENT);
        DL_Timer_setPublisherChanID(TIMA0, DL_TIMER_PUBLISHER_INDEX_0, BSP_EVT_CH_PPG);
    }
    /* TIMA0 is always the cross-trigger master (software source). */
    DL_Timer_configCrossTrigger(TIMA0, DL_TIMER_CROSS_TRIG_SRC_FSUB0,
        DL_TIMER_CROSS_TRIGGER_INPUT_DISABLED, DL_TIMER_CROSS_TRIGGER_MODE_ENABLED);
}

static void timg6_config(void)
{
    timer_power_on(TIMG6);
    timer_base(TIMG6, ECG_PERIOD_TICKS, true);
    DL_Timer_setCaptureCompareValue(TIMG6, ECG_CC, DL_TIMER_CC_0_INDEX);
    DL_Timer_enableEvent(TIMG6, DL_TIMER_EVENT_ROUTE_1, DL_TIMER_EVENT_CC0_DN_EVENT);
    DL_Timer_setPublisherChanID(TIMG6, DL_TIMER_PUBLISHER_INDEX_0, BSP_EVT_CH_ECG);
}

static void timers_stop(void)
{
    DL_Timer_disableExternalTrigger(TIMA0);
    DL_Timer_disableExternalTrigger(TIMG6);
    DL_Timer_stopCounter(TIMA0);
    DL_Timer_stopCounter(TIMG6);
    DL_Timer_disableEvent(TIMA0, DL_TIMER_EVENT_ROUTE_1, 0xFFFFFFFFu);
    DL_Timer_disableEvent(TIMG6, DL_TIMER_EVENT_ROUTE_1, 0xFFFFFFFFu);
}

/* ======================================================================== */
/* ADC helpers                                                              */
/* ======================================================================== */
static void adc_power_on(ADC12_Regs *adc)
{
    DL_ADC12_reset(adc);
    DL_ADC12_enablePower(adc);
    bsp_delay_cycles(32u);
    DL_ADC12_setClockConfig(adc, &k_adc_clk);
    DL_ADC12_setPowerDownMode(adc, DL_ADC12_POWER_DOWN_MODE_MANUAL);
}

static void adc0_ecg_config(void)
{
    static const DL_DMA_Config dma = {
        .transferMode = DL_DMA_FULL_CH_REPEAT_SINGLE_TRANSFER_MODE,
        .extendedMode = DL_DMA_NORMAL_MODE,
        .destIncrement = DL_DMA_ADDR_INCREMENT,
        .srcIncrement = DL_DMA_ADDR_UNCHANGED,
        .destWidth = DL_DMA_WIDTH_HALF_WORD,
        .srcWidth = DL_DMA_WIDTH_HALF_WORD,
        .trigger = DMA_ADC0_EVT_GEN_BD_TRIG,
        .triggerType = DL_DMA_TRIGGER_TYPE_EXTERNAL,
    };

    adc_power_on(ADC0);
    DL_ADC12_initSingleSample(ADC0, DL_ADC12_REPEAT_MODE_ENABLED,
        DL_ADC12_SAMPLING_SOURCE_AUTO, DL_ADC12_TRIG_SRC_EVENT,
        DL_ADC12_SAMP_CONV_RES_12_BIT, DL_ADC12_SAMP_CONV_DATA_FORMAT_UNSIGNED);
    DL_ADC12_configConversionMem(ADC0, DL_ADC12_MEM_IDX_0, ECG_ADC_CHAN,
        DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0,
        DL_ADC12_AVERAGING_MODE_ENABLED, DL_ADC12_BURN_OUT_SOURCE_DISABLED,
        DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configHwAverage(ADC0, DL_ADC12_HW_AVG_NUM_ACC_4, DL_ADC12_HW_AVG_DEN_DIV_BY_4);
    DL_ADC12_setSampleTime0(ADC0, ADC_SAMP_STREAM);
    DL_ADC12_setSubscriberChanID(ADC0, BSP_EVT_CH_ECG);

    DL_DMA_disableChannel(DMA, BSP_DMA_CH_ECG);
    DL_DMA_initChannel(DMA, BSP_DMA_CH_ECG, &dma);
    DL_DMA_setSrcAddr(DMA, BSP_DMA_CH_ECG,
        DL_ADC12_getMemResultAddress(ADC0, DL_ADC12_MEM_IDX_0));
    DL_DMA_setDestAddr(DMA, BSP_DMA_CH_ECG, (uint32_t) &s_ecg_ring[0]);
    DL_DMA_setTransferSize(DMA, BSP_DMA_CH_ECG, (uint16_t) ECG_RING_N);
    DL_DMA_enableChannel(DMA, BSP_DMA_CH_ECG);

    DL_ADC12_setDMASamplesCnt(ADC0, 1u);
    DL_ADC12_enableDMATrigger(ADC0, DL_ADC12_DMA_MEM0_RESULT_LOADED);
    DL_ADC12_enableDMA(ADC0);

    DL_ADC12_clearInterruptStatus(ADC0, 0xFFFFFFFFu);
    DL_ADC12_enableInterrupt(ADC0, DL_ADC12_INTERRUPT_DMA_DONE | DL_ADC12_INTERRUPT_OVERFLOW);
    NVIC_SetPriority(ADC0_INT_IRQn, BSP_PRIO_ADC0);
    NVIC_ClearPendingIRQ(ADC0_INT_IRQn);
    NVIC_EnableIRQ(ADC0_INT_IRQn);

    DL_ADC12_enableConversions(ADC0); /* waits for the first TIMG6 event */
}

static void adc1_ppg_config(void)
{
    adc_power_on(ADC1);
    DL_ADC12_initSeqSample(ADC1, DL_ADC12_REPEAT_MODE_DISABLED,
        DL_ADC12_SAMPLING_SOURCE_AUTO, DL_ADC12_TRIG_SRC_EVENT,
        DL_ADC12_SEQ_START_ADDR_00, DL_ADC12_SEQ_END_ADDR_01,
        DL_ADC12_SAMP_CONV_RES_12_BIT, DL_ADC12_SAMP_CONV_DATA_FORMAT_UNSIGNED);
    DL_ADC12_configConversionMem(ADC1, DL_ADC12_MEM_IDX_0, TIA_ADC_CHAN,
        DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0,
        DL_ADC12_AVERAGING_MODE_ENABLED, DL_ADC12_BURN_OUT_SOURCE_DISABLED,
        DL_ADC12_TRIGGER_MODE_AUTO_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configConversionMem(ADC1, DL_ADC12_MEM_IDX_1, PPGHI_ADC_CHAN,
        DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0,
        DL_ADC12_AVERAGING_MODE_ENABLED, DL_ADC12_BURN_OUT_SOURCE_DISABLED,
        DL_ADC12_TRIGGER_MODE_AUTO_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configHwAverage(ADC1, DL_ADC12_HW_AVG_NUM_ACC_4, DL_ADC12_HW_AVG_DEN_DIV_BY_4);
    DL_ADC12_setSampleTime0(ADC1, ADC_SAMP_STREAM);
    DL_ADC12_setSubscriberChanID(ADC1, BSP_EVT_CH_PPG);

    DL_ADC12_clearInterruptStatus(ADC1, 0xFFFFFFFFu);
    DL_ADC12_enableInterrupt(ADC1, DL_ADC12_INTERRUPT_MEM1_RESULT_LOADED);
    NVIC_SetPriority(ADC1_INT_IRQn, BSP_PRIO_ADC1);
    NVIC_ClearPendingIRQ(ADC1_INT_IRQn);
    NVIC_EnableIRQ(ADC1_INT_IRQn);

    DL_ADC12_enableConversions(ADC1); /* waits for the first TIMA0 event */
}

static void adc_idle(ADC12_Regs *adc, IRQn_Type irq)
{
    NVIC_DisableIRQ(irq);
    DL_ADC12_disableConversions(adc);
    DL_ADC12_disableDMA(adc);
    DL_ADC12_disableInterrupt(adc, 0xFFFFFFFFu);
    DL_ADC12_clearInterruptStatus(adc, 0xFFFFFFFFu);
    DL_ADC12_setPowerDownMode(adc, DL_ADC12_POWER_DOWN_MODE_AUTO);
    NVIC_ClearPendingIRQ(irq);
}

/* ======================================================================== */
/* ISRs                                                                     */
/* ======================================================================== */
void ADC0_IRQHandler(void)
{
    uint32_t iidx;
    while ((iidx = (uint32_t) DL_ADC12_getPendingInterrupt(ADC0)) !=
           ADC12_CPU_INT_IIDX_STAT_NO_INTR) {
        if (iidx == (uint32_t) DL_ADC12_IIDX_DMA_DONE) {
            /* The DMA ring just wrapped (DMADA is back at s_ecg_ring) and the
             * ADC cleared DMAEN. A result that arrived since then is still
             * in MEMRES0 (flag set) and will never be moved: count it lost.
             * Reading MEMRES0 clears MEMRESIFG0. Then re-arm DMAEN. */
            if (DL_ADC12_getRawInterruptStatus(ADC0, DL_ADC12_INTERRUPT_MEM0_RESULT_LOADED)) {
                (void) DL_ADC12_getMemResult(ADC0, DL_ADC12_MEM_IDX_0);
                s_ecg_overruns++;
            }
            DL_ADC12_enableDMA(ADC0);
            s_ecg_laps++;
        } else if (iidx == (uint32_t) DL_ADC12_IIDX_OVERFLOW) {
            s_ecg_overruns++; /* MEMRES0 overwritten before it was moved */
        }
    }
}

void ADC1_IRQHandler(void)
{
    if (DL_ADC12_getPendingInterrupt(ADC1) != DL_ADC12_IIDX_MEM1_RESULT_LOADED)
        return;
    uint32_t ctr = DL_Timer_getTimerCount(TIMA0);
    uint16_t tia = DL_ADC12_getMemResult(ADC1, DL_ADC12_MEM_IDX_0);
    uint16_t hi = DL_ADC12_getMemResult(ADC1, DL_ADC12_MEM_IDX_1);
    /* Non-repeating sequence: re-arm ENC for the next trigger. Toggled, so
     * this works whether the hardware cleared ENC at the end of the sequence
     * (as TI's single-conversion examples imply) or needs an edge on it. */
    DL_ADC12_disableConversions(ADC1);
    DL_ADC12_enableConversions(ADC1);

    /* The counter only moves down from CC_DARK to CC_ON between the dark
     * and the on trigger, so it identifies the phase of this result. */
    bool dark = (ctr <= PPG_CC_DARK) && (ctr > PPG_CC_ON);
    if (dark) {
        if (s_ppg_have_dark) s_ppg_overruns++; /* previous frame lost its on */
        s_ppg_dark_tia = tia;
        s_ppg_dark_hi = hi;
        s_ppg_have_dark = true;
    } else if (s_ppg_have_dark) {
        uint32_t w = s_ppg_wr;
        hal_ppg_frame_t *f = &s_ppg_ring[w % PPG_RING_N];
        f->tia_dark = s_ppg_dark_tia;
        f->hi_dark = s_ppg_dark_hi;
        f->tia_on = tia;
        f->hi_on = hi;
        s_ppg_wr = w + 1u;
        s_ppg_have_dark = false;
    } else {
        s_ppg_overruns++; /* on without dark: this frame is lost */
    }
}

/* ======================================================================== */
/* Streams                                                                  */
/* ======================================================================== */
void hal_acq_start(unsigned streams)
{
    uint32_t pm;

    streams &= (HAL_ACQ_ECG | HAL_ACQ_PPG);
    if (s_running != 0u || streams == 0u) return;

    s_ecg_laps = 0u;
    s_ecg_overruns = 0u;
    s_ecg_rd = 0u;
    s_ecg_last_w = 0u;
    s_ppg_wr = 0u;
    s_ppg_rd = 0u;
    s_ppg_overruns = 0u;
    s_ppg_have_dark = false;

    tima0_config((streams & HAL_ACQ_PPG) != 0u);
    if (streams & HAL_ACQ_ECG) {
        timg6_config();
        adc0_ecg_config();
    }
    if (streams & HAL_ACQ_PPG) {
        adc1_ppg_config();
        /* Hand PA12 to TIMA0_CCP3. The timer is not enabled yet, so the pin
         * shows CCPIV = high: the LEDs stay off until 9.8 ms into frame 0. */
        DL_GPIO_initPeripheralOutputFunction(BSP_LED_BLANK_IOMUX, BSP_LED_BLANK_FUNC);
    }

    pm = hal_irq_lock();
    DL_Timer_generateCrossTrigger(TIMA0); /* t = 0 for every stream */
    s_running = streams;
    hal_irq_unlock(pm);
}

void hal_acq_stop(void)
{
    uint32_t pm = hal_irq_lock();
    bsp_led_blank_gpio_high(); /* first: LEDs off whatever the timer does */
    timers_stop();
    adc_idle(ADC0, ADC0_INT_IRQn);
    adc_idle(ADC1, ADC1_INT_IRQn);
    DL_DMA_disableChannel(DMA, BSP_DMA_CH_ECG);
    s_running = 0u;
    hal_irq_unlock(pm);
}

unsigned hal_acq_running(void)
{
    return s_running;
}

/* Total ECG samples written by the DMA since start, modulo 2^32.
 * Called with IRQs locked, so s_ecg_laps cannot change underneath; the only
 * race is a lap end whose DMA_DONE has not been serviced yet, detected via
 * the raw DMADONE flag (read twice around DMADA until stable) plus a
 * monotonicity check against the previous result. */
static uint32_t ecg_written(void)
{
    const uint32_t base = (uint32_t) &s_ecg_ring[0];
    uint32_t p1, p2, da, idx, w;
    unsigned tries = 0;

    do {
        p1 = DL_ADC12_getRawInterruptStatus(ADC0, DL_ADC12_INTERRUPT_DMA_DONE);
        da = DL_DMA_getDestAddr(DMA, BSP_DMA_CH_ECG);
        p2 = DL_ADC12_getRawInterruptStatus(ADC0, DL_ADC12_INTERRUPT_DMA_DONE);
    } while (p1 != p2 && ++tries < 4u);

    idx = (da - base) / 2u;
    if (idx >= ECG_RING_N) idx = 0u; /* defensive: reload in progress */
    w = (s_ecg_laps + (p2 != 0u ? 1u : 0u)) * ECG_RING_N + idx;
    if ((int32_t) (w - s_ecg_last_w) < 0) w += ECG_RING_N; /* uncounted lap */
    s_ecg_last_w = w;
    return w;
}

size_t hal_ecg_read(uint16_t *dst, size_t max)
{
    uint32_t pm, w, avail, k, i, drop;

    if (dst == NULL || max == 0u) return 0u;
    pm = hal_irq_lock();
    w = ecg_written();
    avail = w - s_ecg_rd;
    if (avail > ECG_RING_N - ECG_SAFETY) {
        /* Reader fell behind: drop the oldest so we never read a slot the
         * DMA is about to overwrite. */
        drop = avail - (ECG_RING_N - ECG_SAFETY);
        s_ecg_rd += drop;
        s_ecg_overruns += drop;
        avail -= drop;
    }
    hal_irq_unlock(pm);

    k = avail < max ? avail : (uint32_t) max;
    for (i = 0; i < k; i++)
        dst[i] = s_ecg_ring[(s_ecg_rd + i) & (ECG_RING_N - 1u)];
    s_ecg_rd += k;
    return k;
}

uint32_t hal_ecg_overruns(void)
{
    return s_ecg_overruns;
}

size_t hal_ppg_read(hal_ppg_frame_t *dst, size_t max)
{
    size_t n = 0;
    if (dst == NULL) return 0u;
    while (n < max) {
        uint32_t pm = hal_irq_lock();
        uint32_t w = s_ppg_wr;
        if (w - s_ppg_rd > PPG_RING_N) { /* oldest frames overwritten */
            s_ppg_overruns += (w - s_ppg_rd) - PPG_RING_N;
            s_ppg_rd = w - PPG_RING_N;
        }
        if (w == s_ppg_rd) {
            hal_irq_unlock(pm);
            break;
        }
        dst[n++] = s_ppg_ring[s_ppg_rd % PPG_RING_N];
        s_ppg_rd++;
        hal_irq_unlock(pm);
    }
    return n;
}

uint32_t hal_ppg_overruns(void)
{
    return s_ppg_overruns;
}

/* ======================================================================== */
/* PPG analog: COMP0 DAC8 bias -> OPA0 (TIA), DAC12 -> OPA1                  */
/* ======================================================================== */
static void opa_wait_ready(OA_Regs *opa)
{
    uint32_t t;
    for (t = 0; t < 100000u && !DL_OPA_isReady(opa); t++) {
    }
}

void hal_ppg_analog(bool on)
{
    if (on) {
        /* COMP0 is enabled only to power its 8-bit DAC (VDDA reference,
         * static mode, software code 0). No comparator input is connected;
         * the DAC8 output feeds OPA0's non-inverting input (OPA0 PSEL =
         * DAC8_OUT, as in TI's opa_dac8_output_buffer example for this LP). */
        static const DL_COMP_Config comp = {
            .channelEnable = DL_COMP_ENABLE_CHANNEL_NONE,
            .mode = DL_COMP_MODE_FAST,
            .negChannel = DL_COMP_IMSEL_CHANNEL_0,
            .posChannel = DL_COMP_IPSEL_CHANNEL_0,
            .hysteresis = DL_COMP_HYSTERESIS_NONE,
            .polarity = DL_COMP_POLARITY_NON_INV,
        };
        static const DL_COMP_RefVoltageConfig comp_ref = {
            .mode = DL_COMP_REF_MODE_STATIC,
            .source = DL_COMP_REF_SOURCE_VDDA_DAC,
            .terminalSelect = DL_COMP_REF_TERMINAL_SELECT_NEG,
            .controlSelect = DL_COMP_DAC_CONTROL_SW,
            .inputSelect = DL_COMP_DAC_INPUT_DACCODE0,
        };
        /* OPA0: general purpose, IN0- (PA27) inverting, DAC8 non-inverting,
         * output to PA22, chopping on, high GBW. No internal gain network. */
        static const DL_OPA_Config opa0 = {
            .pselChannel = DL_OPA_PSEL_DAC8_OUT,
            .nselChannel = DL_OPA_NSEL_IN0_NEG,
            .mselChannel = DL_OPA_MSEL_OPEN,
            .gain = DL_OPA_GAIN_N0_P1,
            .outputPinState = DL_OPA_OUTPUT_PIN_ENABLED,
            .choppingMode = DL_OPA_CHOPPING_MODE_STANDARD,
        };
        /* DAC12: VDDA reference, 12-bit straight binary, buffered output. */
        static const DL_DAC12_Config dac = {
            .outputEnable = DL_DAC12_OUTPUT_ENABLED,
            .resolution = DL_DAC12_RESOLUTION_12BIT,
            .representation = DL_DAC12_REPRESENTATION_BINARY,
            .voltageReferenceSource = DL_DAC12_VREF_SOURCE_VDDA_VSSA,
            .amplifierSetting = DL_DAC12_AMP_ON,
            .fifoEnable = DL_DAC12_FIFO_DISABLED,
            .fifoTriggerSource = DL_DAC12_FIFO_TRIGGER_SAMPLETIMER,
            .dmaTriggerEnable = DL_DAC12_DMA_TRIGGER_DISABLED,
            .dmaTriggerThreshold = DL_DAC12_FIFO_THRESHOLD_ONE_QTR_EMPTY,
            .sampleTimeGeneratorEnable = DL_DAC12_SAMPLETIMER_DISABLE,
            .sampleRate = DL_DAC12_SAMPLES_PER_SECOND_500,
        };
        /* OPA1: IN1- (PA17) inverting, DAC12 non-inverting, output PA16,
         * rail-to-rail input. */
        static const DL_OPA_Config opa1 = {
            .pselChannel = DL_OPA_PSEL_DAC_OUT,
            .nselChannel = DL_OPA_NSEL_IN1_NEG,
            .mselChannel = DL_OPA_MSEL_OPEN,
            .gain = DL_OPA_GAIN_N0_P1,
            .outputPinState = DL_OPA_OUTPUT_PIN_ENABLED,
            .choppingMode = DL_OPA_CHOPPING_MODE_DISABLE,
        };

        DL_COMP_init(COMP0, (DL_COMP_Config *) &comp);
        DL_COMP_refVoltageInit(COMP0, (DL_COMP_RefVoltageConfig *) &comp_ref);
        DL_COMP_setDACCode0(COMP0, PPG_PD_BIAS_DAC8);
        DL_COMP_enable(COMP0);

        DL_OPA_init(OPA0, &opa0);
        DL_OPA_setGainBandwidth(OPA0, DL_OPA_GBW_HIGH);
        DL_OPA_enable(OPA0);
        opa_wait_ready(OPA0);

        DL_DAC12_init(DAC0, &dac);
        DL_DAC12_output12(DAC0, s_dac12_code);
        DL_DAC12_enable(DAC0);

        DL_OPA_init(OPA1, &opa1);
        DL_OPA_setGainBandwidth(OPA1, DL_OPA_GBW_HIGH);
        DL_OPA_enableRailToRailInput(OPA1);
        DL_OPA_enable(OPA1);
        opa_wait_ready(OPA1);
        s_analog_on = true;
    } else {
        /* Reverse order. Resetting puts each block back to its power-on
         * register state (outputs disconnected). The GPAMP is never touched
         * except to make sure it is off. */
        s_analog_on = false;
        DL_OPA_disable(OPA1);
        DL_DAC12_disable(DAC0);
        DL_OPA_disable(OPA0);
        DL_COMP_disable(COMP0);
        DL_OPA_reset(OPA1);
        DL_DAC12_reset(DAC0);
        DL_OPA_reset(OPA0);
        DL_COMP_reset(COMP0);
        DL_OPA_enablePower(OPA0);
        DL_OPA_enablePower(OPA1);
        DL_DAC12_enablePower(DAC0);
        DL_COMP_enablePower(COMP0);
        bsp_delay_cycles(32u);
    }
}

void hal_ppg_set_offset(uint16_t dac12_code)
{
    if (dac12_code > 4095u) dac12_code = 4095u;
    s_dac12_code = dac12_code;
    if (s_analog_on) DL_DAC12_output12(DAC0, dac12_code);
}

/* ======================================================================== */
/* One-shot conversions                                                     */
/* ======================================================================== */
uint16_t hal_adc_read_once(hal_adc_ch_t ch)
{
    ADC12_Regs *adc;
    uint32_t chan, t;
    uint16_t v;

    if (s_running != 0u) return 0u;
    switch (ch) {
    case HAL_ADC_ECG:    adc = ADC0; chan = ECG_ADC_CHAN; break;
    case HAL_ADC_TIA:    adc = ADC1; chan = TIA_ADC_CHAN; break;
    case HAL_ADC_PPG_HI: adc = ADC1; chan = PPGHI_ADC_CHAN; break;
    case HAL_ADC_VMID:   adc = ADC1; chan = VMID_ADC_CHAN; break;
    default: return 0u;
    }

    adc_power_on(adc);
    DL_ADC12_initSingleSample(adc, DL_ADC12_REPEAT_MODE_DISABLED,
        DL_ADC12_SAMPLING_SOURCE_AUTO, DL_ADC12_TRIG_SRC_SOFTWARE,
        DL_ADC12_SAMP_CONV_RES_12_BIT, DL_ADC12_SAMP_CONV_DATA_FORMAT_UNSIGNED);
    DL_ADC12_configConversionMem(adc, DL_ADC12_MEM_IDX_0, chan,
        DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0,
        DL_ADC12_AVERAGING_MODE_ENABLED, DL_ADC12_BURN_OUT_SOURCE_DISABLED,
        DL_ADC12_TRIGGER_MODE_AUTO_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    /* 16 conversions accumulated and divided by 16 in hardware. */
    DL_ADC12_configHwAverage(adc, DL_ADC12_HW_AVG_NUM_ACC_16, DL_ADC12_HW_AVG_DEN_DIV_BY_16);
    DL_ADC12_setSampleTime0(adc, ADC_SAMP_ONESHOT);
    DL_ADC12_clearInterruptStatus(adc, DL_ADC12_INTERRUPT_MEM0_RESULT_LOADED);
    DL_ADC12_enableConversions(adc);
    DL_ADC12_startConversion(adc);
    /* 16 x (10 us + ~0.5 us) ~= 170 us; allow ~20x. */
    for (t = 0; t < 100000u; t++) {
        if (DL_ADC12_getRawInterruptStatus(adc, DL_ADC12_INTERRUPT_MEM0_RESULT_LOADED))
            break;
    }
    v = DL_ADC12_getMemResult(adc, DL_ADC12_MEM_IDX_0);
    DL_ADC12_disableConversions(adc);
    DL_ADC12_setPowerDownMode(adc, DL_ADC12_POWER_DOWN_MODE_AUTO);
    return (uint16_t) (v & 0x0FFFu);
}

/* ======================================================================== */
/* Power-on state                                                           */
/* ======================================================================== */
void bsp_acq_init(void)
{
    DL_ADC12_reset(ADC0);
    DL_ADC12_reset(ADC1);
    DL_ADC12_enablePower(ADC0);
    DL_ADC12_enablePower(ADC1);
    timer_power_on(TIMA0);
    timer_power_on(TIMG6);
    DL_COMP_reset(COMP0);
    DL_OPA_reset(OPA0);
    DL_OPA_reset(OPA1);
    DL_DAC12_reset(DAC0);
    DL_COMP_enablePower(COMP0);
    DL_OPA_enablePower(OPA0);
    DL_OPA_enablePower(OPA1);
    DL_DAC12_enablePower(DAC0);
    bsp_delay_cycles(32u);
    /* PA22 is shared with the GPAMP output: make sure it is off. */
    DL_GPAMP_disableOutputToPad();
    DL_GPAMP_disable();
    s_running = 0u;
    s_analog_on = false;
}
