/* ecg_dsp: fixed-point signal processing for the ECG card (pure C11, no
 * hardware access, no globals, no malloc, no floating point). All state lives
 * in caller-owned structs. See fw/dsp/README.md for units, call order,
 * group delays, RAM and CPU budgets. */
#ifndef ECG_DSP_H
#define ECG_DSP_H

#include "dsp_biquad.h"
#include "dsp_decim.h"
#include "dsp_delays.h"
#include "dsp_ecg_disp.h"
#include "dsp_ecg_mon.h"
#include "dsp_hrv.h"
#include "dsp_mains.h"
#include "dsp_pat.h"
#include "dsp_ppg.h"
#include "dsp_ppg_ctrl.h"
#include "dsp_qrs.h"

#endif
