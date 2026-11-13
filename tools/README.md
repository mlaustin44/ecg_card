# Host tools

Python tools for the ECG Card stream: decoder, recorder, plotter, synthetic signal
generator, and floating-point reference DSP. Needs numpy, scipy, matplotlib and pytest
(`requirements.txt`; pyserial only for the future UART). From the repository root:

    python3 -m venv --system-site-packages .venv && .venv/bin/pip install -r tools/requirements.txt
    .venv/bin/python -m pytest tools/tests -q

(`--system-site-packages` reuses numpy/scipy/matplotlib from the system; the venv is only
needed if pytest or matplotlib are missing.) All scripts run from any directory.

## Package `tools/ecgcard/`

| Module | Contents |
|---|---|
| `codec.py` | COBS, CRC-16/CCITT-FALSE, frame pack/unpack, message types, `StreamEncoder`, incremental `StreamDecoder` |
| `record.py` | `RecordingBuilder`/`Recording`: ECG by start index (gaps are NaN), uV conversion, PPG fine unwrap, events, `.npz` and CSV |
| `capture.py`, `sources.py` | file / tail / TCP / serial sources, live statistics |
| `report.py` | offline PNG report (no pyplot, headless) |
| `siggen.py` | synthetic ECG + PPG with ground truth |
| `refdsp.py` | reference decimator, display filter, Pan-Tompkins, HRV, PPG foot and PAT |
| `streamsim.py` | build the byte stream the firmware would send from a simulation (firmware timeline and fine) |

### Conventions (as the firmware, `fw/dsp/README.md` and `fw/dsp/include/dsp_delays.h`)

Times are seconds from the common acquisition start event.

* **ECG timeline.** Stream sample index `m` (ECG_BLOCK `start + i`, the EV_R_PEAK index,
  and the EV_PULSE foot index, which is in ECG sample units) is the signal at
  `t = m/500 + ECG_T0_S`, `ECG_T0_S = -0.010` (`DSP_ECG_STREAM_T0_US`: the decimator FIR
  delay stays in the stream). `record.ecg_time()`, `Recording.ecg_t`, `r_peak_times()` and
  `pulse_foot_times()` use this. A true R-peak is a further `ECG_ANALOG_DELAY_S = 0.000974`
  s earlier (`DSP_ECG_ANALOG_DELAY_US`, analog front end): `record.r_true_time()` and
  `Recording.r_peak_true_times()` remove it, and any PAT the tools compute (streamsim's
  reference path) is foot minus true R time, as the firmware's `pat_ms`. Constants live in
  `ecgcard/record.py`. (`refdsp.decimate_8` itself is zero-delay; `streamsim.stream_ecg`
  moves it onto the stream timeline, so a simulated R at `t_R` is sent at index
  `round((t_R + 0.000974 + 0.010) * 500)`, within a sample.)
* **PPG timeline.** Frame `k`'s LED-on sample is at `k*0.01 + 0.00995` s.
* **PPG fine sign.** `fine = dsp_ppg_fine() = hi_on - 11*dac12 + dark10`, with
  `dark10 = 11*dac12 - hi_dark` when `hi_dark` is in 16..4079 (then `fine = hi_on - hi_dark`),
  else `10*tia_dark`; if `hi_on` is outside 16..4079, `fine = -10*coarse`. So fine is
  **minus** the LED-induced photocurrent in 80.6 pA units: more blood, less light, fine goes
  **up**. The systolic upstroke points up in the raw stream, and the tools plot and analyse
  fine as is (`Recording.ppg_fine`, alias `ppg_systole_up`; no negation). It wraps modulo
  2^16 on the wire and `Recording.ppg_fine` unwraps it. `siggen.fine_from_frames` is the
  same formula.

Decoder counters: `bad_crc`,
`bad_length`, `bad_cobs`, `unknown_type`, `seq_gaps` (places where the counter jumped),
`seq_lost` (frames missing by the counter; a CRC-dropped frame also shows up here),
`seq_repeats` (duplicate or reordered frames, not counted as loss).

## Live capture on the laptop (J-Link, RTT channel 1)

Terminal 1, log RTT channel 1 raw bytes to a file (the target must be running; the
logger prompts nothing in this form):

    JLinkRTTLogger -Device MSPM0G3507 -If SWD -Speed 4000 -RTTChannel 1 build/rtt.bin

Terminal 2, decode while it grows:

    python3 tools/stream_capture.py --follow build/rtt.bin --raw build/session.bin --npz build/rec.npz
    python3 tools/stream_plot.py    --follow build/rtt.bin --window 8

Ctrl-C ends a capture and writes the `.npz`. If the logger started mid-frame the decoder
resynchronises at the next 0x00 delimiter by itself. For a quick check of a finished log:
`python3 tools/stream_plot.py --report build/rtt.bin --png build/report.png`.

If `JLinkRTTLogger` cannot find the RTT control block, check your J-Link software's `JLinkRTTLogger` help for an address / search-range option (not verified here) and give it the `_SEGGER_RTT` address from the map file.

## `stream_capture.py`

    stream_capture.py (--file F | --follow F | --tcp HOST:PORT | --serial PORT [--baud N])
                      [--raw OUT.bin] [--npz OUT.npz] [--csv BASE] [--duration S] [--tail-only]
                      [--idle-timeout S] [--stats-every S] [-q]

Prints once per second: frames, ECG/PPG blocks, bad CRC/length/COBS, unknown types,
sequence losses, duplicates, frames/s, bytes/s, last HR and PAT. `--raw` saves the bytes
as received; `--npz` the decoded `Recording`; `--csv BASE` writes `BASE.ecg.csv`,
`BASE.ppg.csv`, `BASE.events.csv`. `--follow` waits up to 30 s for the file to appear.
`--tcp` takes any socket carrying the raw stream (for example `socat` from a UART).

## `stream_plot.py`

Live (needs a display): two scrolling panels, ECG in uV with R-peak markers and PPG
(systole up) with pulse-foot markers, HR/PAT/error counters in the corner.

    stream_plot.py (--follow F | --file F | --tcp HOST:PORT | --serial PORT) [--window S] [--raw OUT] [--npz OUT]

Offline PNG report (headless, uses Agg; INPUT is a `.npz` or a raw stream file):

    stream_plot.py --report INPUT [--png OUT.png] [--zoom 5]

The report has full ECG and PPG traces, a zoomed window with R-peaks and pulse feet,
the 0-250 Hz ECG amplitude spectrum (dotted lines at 50/60 Hz and harmonics), the RR
tachogram, and a table (HR mean/min/max, SDNN, RMSSD, missing samples, lost frames,
CRC errors, PAT). R-peaks come from device events, or from the reference detector if
the stream has none.

## `check_capture.py`

    check_capture.py FILE [--min-ecg-blocks N] [--min-ppg-blocks N] [--require-events KIND,...] [-q]

Decodes a raw capture (COBS frames + 0x00, as the device writes them) and prints frames,
per-type counts, `bad_crc`/`bad_length`/`bad_cobs`/`unknown_type`, `seq_lost`, the number
of ECG_BLOCK (step 10) and PPG_BLOCK (step 2) start-index gaps, event counts, and mean HR,
pulse rate and PAT from the events. KIND is `r_peak`, `pulse`, `lead`, `state` or
`button`. Exit 0 if every check passes, 1 if any CRC/length/COBS error occurs, block
indices are not contiguous within a run (a start index of 0 begins a new run and is
fine), a minimum is not met or a required event kind is absent, 2 on a usage error or an
unreadable file. Lost sequence numbers, unknown types and trailing bytes are reported
only. `-q` prints nothing but `FAIL:` lines (stderr).

## Fixtures: `make_fixtures.py`

    python3 tools/make_fixtures.py [--out build/fixtures] [--only NAME ...] [--list]

Deterministic (fixed seeds), about 4 MB in total. Names: clean_60bpm, fast_180bpm,
slow_40bpm, low_amp_noisy (0.25 mV R, EMG noise, gain 1098), mains50, mains60 (300 uV at
the fundamental plus harmonics), wander, tall_t, hrv (60 s, SDNN about 25 ms with RSA),
both_pat200, ectopic (every 7th beat premature), gain1098, led_step (LED current step at
frame 1500).

Each fixture `<name>` is three files, readable from C without Python:

* `<name>.ecg.u16`: raw little-endian `uint16` ADC codes at 4000 Hz (12-bit, 0..4095),
  centred on `ecg_mid_code` in the JSON. Input-referred microvolts are
  `(code - mid) * 805.7 / gain`. Sample `n` is at `n / 4000` s.
* `<name>.ppg.u16`: little-endian `uint16`, four values per 100 Hz frame:
  `tia_dark, hi_dark, tia_on, hi_on` (12-bit codes, 3.3 V reference). Frame `k` starts at
  `k * 0.01` s; the dark sample is at +9.70 ms and the on sample at +9.95 ms.
  `TIA_OUT = 0.245 V + I * 1 MOhm`; `PPG_HI = 11 * V_DAC12 - 10 * TIA_OUT`, clipped to
  0..3.3 V (it rails high in the dark phase, as in hardware). Stream values:
  `coarse = tia_on - tia_dark`; `fine` as the firmware computes it (see Conventions:
  `hi_on - 11 * dac12 + 10 * tia_dark` here, since `hi_dark` rails), which is minus the
  LED photocurrent in 80.6 pA units (systole up) and continuous across DAC12 steps (wraps
  to int16 on the wire).
* `<name>.json`: parameters and ground truth. Keys: `ecg_samples`, `ppg_frames`,
  `ecg_mid_code`, `ecg_gain` (558 or 1098), `uv_per_code`, `ppg_led_code_schedule` and
  `ppg_dac12_schedule` (`[[first_frame, code], ...]`; LED current = code * 3.718 uA),
  `params` (all generator parameters including seeds), and `truth`:
  `r_peak_times_s`, `ectopic` (flag per beat), `rr_s` (true intervals, ectopic included),
  `pulse_foot_times_s` (intersecting-tangent foot = R + PAT), `pat_s`, `hr_bpm`, `sdnn_ms`,
  `rmssd_ms` (computed over normal-to-normal intervals only, i.e. excluding intervals next
  to ectopic beats; SDNN uses n-1), `pat_mean_s`.

The generator: P-QRS-T as a sum of Gaussians (R amplitude is input-referred, mains and
baseline wander are added before the 0.5 Hz / 150 Hz analog band, so the stated amplitudes
are before that band; that band shifts the R peak by about 1.07 ms, close to the 0.974 ms
the firmware assumes). Photocurrent = ambient (optional 100/120 Hz flicker, seen by both
dark and on samples) + LED DC * (1 - PI * pulse), so the pulse lowers the light and
raises fine.
Disturbed flags mark frames where the LED code or DAC12 changed.

## Reference DSP (`refdsp.py`)

`decimate_8` (equiripple FIR, passband 150 Hz, over 60 dB at 250 Hz, group delay removed),
`display_filter` (0.5 Hz HP, 50/60 Hz notch Q=30, 40 Hz LP), `pan_tompkins`,
`hrv_stats`, `ppg_feet` (intersecting tangents on a 1 kHz spline, 10 Hz low-pass),
`pat_from_feet`, `match_events`. They define the numbers the fixed-point firmware is
compared against; accuracy on the fixtures is asserted in `tests/`.

## Tests

`.venv/bin/python -m pytest tools/tests -q`: codec (COBS/CRC vectors, boundaries,
fuzzing), record assembly and timeline, generator ground truth and the firmware fine
formula, reference DSP accuracy, `check_capture.py` (good, corrupted, gap, new run,
minimums), and an end-to-end run (generator -> reference DSP -> byte stream on the
firmware timeline -> capture CLI -> report PNG, plus tail-follow and TCP sources).
