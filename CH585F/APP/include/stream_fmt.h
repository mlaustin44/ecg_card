/* Stream frame format. The same frames go to RTT channel 1 today and to the
 * radio UART later (ecg-fw.md section 5).
 *
 * On the wire: COBS(frame) followed by one 0x00 delimiter.
 * Decoded frame, little-endian:
 *   u8  type
 *   u8  seq        one counter for all frame types, +1 per frame sent (wraps)
 *   u16 len        payload length, 0..512
 *   u8  payload[len]
 *   u16 crc        CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect,
 *                  xorout 0) over type, seq, len and payload
 *
 * STREAM_ECG_BLOCK (0x10), 25 bytes, every 20 ms:
 *   u32 start      index of the first sample (500 Hz, from acquisition start)
 *   u8  flags      bit0 gain_hi (1098x), bit1 settling, bit2 lead_off
 *   i16 s[10]      decimated ADC codes relative to mid-scale, times 8:
 *                  microvolts = s * 805.7 / (8 * gain), gain = 558 or 1098
 *
 * STREAM_PPG_BLOCK (0x11), 15 bytes, every 20 ms:
 *   u32 start      index of the first frame (100 Hz, from acquisition start)
 *   u16 led_code   MCP4728 channel A code (LED current = code * 3.718 uA)
 *   u8  flags      bit0 frame 0 disturbed, bit1 frame 1 disturbed
 *                  (AGC or offset step), bit2 no finger
 *   { i16 coarse; i16 fine; } f[2]
 *                  coarse = tia_on - tia_dark (ADC codes, 0.81 nA per code)
 *                  fine   = dsp_ppg_fine(): minus the LED-induced
 *                           photocurrent in 80.6 pA units, so the systolic
 *                           upstroke points up; continuous across DAC12
 *                           offset steps; wraps modulo 2^16
 *
 * STREAM_EVENT (0x12): u8 kind, then kind-specific payload:
 *   EV_R_PEAK    (1): u32 sample index (500 Hz), u16 hr_bpm
 *   EV_PULSE     (2): u32 foot time in ECG sample units (500 Hz), u16 pulse_bpm,
 *                     u16 pat_ms (0 = n/a), u16 pi_permille
 *   EV_LEAD      (3): u8 contact (0 off, 1 on)
 *   EV_STATE     (4): u8 mode (0 idle, 1 ECG, 2 PPG, 3 both), u8 notch_hz,
 *                     u16 mid_code, u8 batt_pct, u8 flags (bit0 usb, bit1 charging)
 *   EV_BUTTON    (5): u8 button (1 or 2), u8 kind (0 short, 1 long)
 *
 * STREAM_LOG (0x30): UTF-8 text, no terminator. */
#ifndef STREAM_FMT_H
#define STREAM_FMT_H

#define STREAM_ECG_BLOCK 0x10u
#define STREAM_PPG_BLOCK 0x11u
#define STREAM_EVENT     0x12u
#define STREAM_LOG       0x30u

#define STREAM_EV_R_PEAK 1u
#define STREAM_EV_PULSE  2u
#define STREAM_EV_LEAD   3u
#define STREAM_EV_STATE  4u
#define STREAM_EV_BUTTON 5u

#define STREAM_ECG_SAMPLES_PER_BLOCK 10u
#define STREAM_PPG_FRAMES_PER_BLOCK  2u
#define STREAM_ECG_SCALE             8   /* stream units per ADC code */
#define STREAM_MAX_PAYLOAD           512u

#endif
