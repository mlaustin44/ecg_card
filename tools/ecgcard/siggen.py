"""Synthetic ECG + PPG generator with ground truth, rendered as the firmware's ADCs see it.

ECG: P-QRS-T sum of Gaussians -> input-referred noise/interference -> analog band
(0.5 Hz 1st-order HP, 150 Hz 2nd-order LP) -> gain -> 12-bit codes at 4000 Hz around `mid_code`.
PPG: pulse wave per beat delayed by PAT from the R-peak -> photocurrent -> TIA -> PPG_HI stage
-> 12-bit codes, as 100 Hz frames {tia_dark, hi_dark, tia_on, hi_on}.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, field

import numpy as np
from scipy import signal

from .hrv import hrv_stats

ECG_FS = 4000
PPG_FS = 100
UV_PER_CODE = 805.7
VREF = 3.3
ADC_FS_CODES = 4096
TIA_BIAS_V = 0.245
TIA_OHMS = 1.0e6
LED_UA_PER_CODE = 3.718
T_DARK_S = 0.0097
T_ON_S = 0.00995
FIRST_BEAT_S = 1.0


def volts_to_code(v: np.ndarray) -> np.ndarray:
    return np.clip(np.round(np.asarray(v) / VREF * ADC_FS_CODES), 0, ADC_FS_CODES - 1).astype(np.int64)


@dataclass
class EcgParams:
    duration_s: float = 30.0
    hr_bpm: float = 60.0
    rr_sd_ms: float = 0.0          # beat-to-beat Gaussian jitter
    rsa_ms: float = 0.0            # respiratory sinus arrhythmia amplitude (ms)
    rsa_hz: float = 0.25
    r_mv: float = 1.0              # R amplitude, input-referred
    p_ratio: float = 0.12          # wave amplitudes relative to R
    t_ratio: float = 0.30
    wander_mv: float = 0.0         # baseline wander amplitude before the 0.5 Hz high-pass
    mains_hz: float = 0.0          # 0, 50 or 60
    mains_uv: float = 0.0          # fundamental amplitude, input-referred
    mains_harmonics: tuple[float, ...] = (1.0, 0.3, 0.15)  # relative amplitude of 1x, 2x, 3x ...
    emg_uv_rms: float = 0.0        # white noise before the analog band, input-referred
    adc_noise_codes: float = 0.4   # rms noise added in the ADC
    ectopic_every: int = 0         # every n-th beat is a premature ventricular beat (0 = none)
    gain: int = 558                # 558 or 1098
    mid_code: int = 2048
    seed: int = 1


@dataclass
class PpgParams:
    led_ma: float = 5.0
    pat_ms: float = 250.0          # R-peak to pulse foot (intersecting tangents)
    pat_sd_ms: float = 0.0
    dc_ua_per_ma: float = 0.3      # DC photocurrent per mA of LED current
    pi_percent: float = 2.0        # pulsatile part of the LED photocurrent (decreases light in systole)
    ambient_ua: float = 0.05
    flicker_hz: float = 0.0        # 0, 100 or 120
    flicker_depth: float = 0.3
    noise_na: float = 1.0          # rms photocurrent noise per sample
    dac_period_frames: int = 100   # DC-cancel update interval (about once per second)
    led_steps: tuple[tuple[int, int], ...] = ()  # (frame, led code) changes after frame 0
    seed: int = 2


# ---------------------------------------------------------------- beats

# (centre offset s, width s, amplitude relative to R) for a normal beat at RR = 1 s
_WAVES = {
    "P": (-0.180, 0.025, None),
    "Q": (-0.028, 0.010, -0.12),
    "R": (0.0, 0.012, 1.0),
    "S": (0.030, 0.012, -0.20),
    "T": (0.250, 0.050, None),
}


@dataclass
class BeatSchedule:
    r_times: np.ndarray      # s
    rr: np.ndarray           # s, len = n - 1 (true intervals)
    ectopic: np.ndarray      # bool per beat


def make_beats(p: EcgParams, rng: np.random.Generator) -> BeatSchedule:
    mean_rr = 60.0 / p.hr_bpm
    times = [FIRST_BEAT_S]
    ect = [False]
    k = 0
    end = p.duration_s + 1.0
    while times[-1] < end:
        k += 1
        rr = mean_rr + rng.normal(0, p.rr_sd_ms / 1000.0) + p.rsa_ms / 1000.0 * np.sin(2 * np.pi * p.rsa_hz * times[-1])
        rr = max(rr, 0.25)
        if p.ectopic_every and (k + 1) % p.ectopic_every == 0:
            # premature beat, then compensatory pause (the two intervals sum to 2 RR)
            times.append(times[-1] + 0.65 * rr)
            ect.append(True)
            times.append(times[-1] + 1.35 * rr)
            ect.append(False)
            k += 1
        else:
            times.append(times[-1] + rr)
            ect.append(False)
    t = np.array(times)
    keep = t < p.duration_s - 0.5  # leave room so the T wave of the last beat is complete
    t, e = t[keep], np.array(ect)[keep]
    return BeatSchedule(t, np.diff(t), e)


def ecg_clean_mv(p: EcgParams, beats: BeatSchedule, t: np.ndarray) -> np.ndarray:
    """Noise-free ECG in mV on time axis t."""
    x = np.zeros_like(t)
    for i, tr in enumerate(beats.r_times):
        rr = beats.rr[i] if i < len(beats.rr) else 60.0 / p.hr_bpm
        s = float(np.clip(np.sqrt(rr), 0.55, 1.2))  # intervals shrink with rate
        lo = int(max((tr - 0.5) * ECG_FS, 0))
        hi = int(min((tr + 0.7) * ECG_FS, len(t)))
        if hi <= lo:
            continue
        tt = t[lo:hi] - tr
        ect = beats.ectopic[i]
        for name, (c, w, a) in _WAVES.items():
            if name == "P":
                if ect:
                    continue
                a = p.p_ratio
            if name == "T":
                a = -p.t_ratio if ect else p.t_ratio
            ww = w * (2.2 if (ect and name in "QRS") else 1.0)
            amp = a * (1.3 if (ect and name == "R") else 1.0)
            cc = c * s * (1.4 if (ect and name == "T") else 1.0)
            x[lo:hi] += amp * p.r_mv * np.exp(-0.5 * ((tt - cc) / (ww * (s if name in "PT" else 1.0))) ** 2)
    return x


def _analog_band(x_uv: np.ndarray) -> np.ndarray:
    sos_hp = signal.butter(1, 0.5, "highpass", fs=ECG_FS, output="sos")
    sos_lp = signal.butter(2, 150, "lowpass", fs=ECG_FS, output="sos")
    sos = np.vstack([sos_hp, sos_lp])
    zi = signal.sosfilt_zi(sos) * x_uv[0]
    return signal.sosfilt(sos, x_uv, zi=zi)[0]


def render_ecg(p: EcgParams, beats: BeatSchedule, rng: np.random.Generator) -> np.ndarray:
    n = int(round(p.duration_s * ECG_FS))
    t = np.arange(n) / ECG_FS
    uv = ecg_clean_mv(p, beats, t) * 1000.0
    if p.wander_mv:
        ph = rng.uniform(0, 2 * np.pi, 2)
        uv += 1000.0 * p.wander_mv * (0.7 * np.sin(2 * np.pi * 0.17 * t + ph[0]) + 0.3 * np.sin(2 * np.pi * 0.31 * t + ph[1]))
    if p.mains_hz and p.mains_uv:
        for h, rel in enumerate(p.mains_harmonics, start=1):
            uv += p.mains_uv * rel * np.sin(2 * np.pi * p.mains_hz * h * t + rng.uniform(0, 2 * np.pi))
    if p.emg_uv_rms:
        uv += rng.normal(0, p.emg_uv_rms, n)
    uv = _analog_band(uv)
    codes = p.mid_code + uv * p.gain / UV_PER_CODE
    if p.adc_noise_codes:
        codes = codes + rng.normal(0, p.adc_noise_codes, n)
    return np.clip(np.round(codes), 0, ADC_FS_CODES - 1).astype(np.uint16)


# ---------------------------------------------------------------- PPG

def pulse_template(fs: float = 2000.0) -> tuple[np.ndarray, float]:
    """Normalised pulse (0 -> 1 at systolic peak, with dicrotic notch), onset at t = 0.

    Returns (samples at fs, tangent_foot_offset_s): the intersecting-tangent foot lies
    that much AFTER the onset. The generator aligns this foot to R + PAT.
    """
    t = np.arange(0, 1.0, 1.0 / fs)
    ts = 0.16  # rise time
    rise = 0.5 * (1 - np.cos(np.pi * np.clip(t / ts, 0, 1)))
    decay = np.where(t > ts, np.exp(-(t - ts) / 0.22), 1.0)
    notch = 0.18 * np.exp(-0.5 * ((t - 0.42) / 0.05) ** 2)
    pw = rise * decay + notch * (t > 0.25)
    pw /= pw.max()
    # intersecting tangents on the rising edge
    k = int(np.argmax(np.gradient(pw, t)[: int(0.3 * fs)]))
    slope = np.gradient(pw, t)[k]
    foot = t[k] - (pw[k] - pw[0]) / slope
    return pw, float(foot)


@dataclass
class PpgSignal:
    frames: np.ndarray        # uint16 (n, 4): tia_dark, hi_dark, tia_on, hi_on
    fine: np.ndarray          # int64 true fine value (81 pA units, 10 x TIA code above bias), unwrapped
    dac12: np.ndarray         # uint16 per frame
    led_code: np.ndarray      # uint16 per frame
    disturbed: np.ndarray     # bool per frame (LED or DAC12 step)
    foot_times: np.ndarray    # s, one per beat that produced a pulse
    pat_s: np.ndarray         # s per foot


def render_ppg(p: PpgParams, beats: BeatSchedule, duration_s: float, rng: np.random.Generator) -> PpgSignal:
    n = int(round(duration_s * PPG_FS))
    k = np.arange(n)
    t_on = k / PPG_FS + T_ON_S
    t_dark = k / PPG_FS + T_DARK_S

    pat = p.pat_ms / 1000.0 + rng.normal(0, p.pat_sd_ms / 1000.0, len(beats.r_times))
    foot = beats.r_times + pat
    tpl, foot_off = pulse_template()
    fs_t = 2000.0
    pulse = np.zeros(n)
    for f in foot:
        onset = f - foot_off
        idx = np.flatnonzero((t_on > onset) & (t_on < onset + 1.0))
        if idx.size:
            pulse[idx] += np.interp(t_on[idx] - onset, np.arange(len(tpl)) / fs_t, tpl)

    led_code = np.full(n, int(round(p.led_ma * 1000.0 / LED_UA_PER_CODE)), dtype=np.int64)
    for fr, code in p.led_steps:
        led_code[fr:] = code
    led_ma = led_code * LED_UA_PER_CODE / 1000.0

    def ambient(t: np.ndarray) -> np.ndarray:
        a = np.full(len(t), p.ambient_ua)
        if p.flicker_hz:
            a = a * (1 + p.flicker_depth * np.sin(2 * np.pi * p.flicker_hz * t + 0.7))
        return a

    i_led_ua = p.dc_ua_per_ma * led_ma * (1 - p.pi_percent / 100.0 * pulse)
    noise = lambda: rng.normal(0, p.noise_na / 1000.0, n)
    i_dark = ambient(t_dark) + noise()
    i_on = ambient(t_on) + i_led_ua + noise()
    tia_dark_v = np.clip(TIA_BIAS_V + i_dark * 1e-6 * TIA_OHMS, 0, VREF)
    tia_on_v = np.clip(TIA_BIAS_V + i_on * 1e-6 * TIA_OHMS, 0, VREF)

    # DC cancellation: V_DAC12 = (1.65 + 10 TIA_on) / 11, updated once per period from the
    # previous period's mean (the first period uses its own mean, as a start-up estimate).
    dac = np.zeros(n, dtype=np.int64)
    per = p.dac_period_frames
    for s in range(0, n, per):
        ref = tia_on_v[max(s - per, 0) if s else 0: s if s else per]
        dac[s:s + per] = volts_to_code((1.65 + 10 * ref.mean()) / 11)
    v_dac = dac / ADC_FS_CODES * VREF
    hi_dark_v = np.clip(11 * v_dac - 10 * tia_dark_v, 0, VREF)
    hi_on_v = np.clip(11 * v_dac - 10 * tia_on_v, 0, VREF)

    tia_dark, tia_on = volts_to_code(tia_dark_v), volts_to_code(tia_on_v)
    hi_dark, hi_on = volts_to_code(hi_dark_v), volts_to_code(hi_on_v)
    # fine = 11 * dac - hi_on - bias: continuous across DAC steps (10 x TIA code above the bias)
    bias_codes = int(round(10 * TIA_BIAS_V / VREF * ADC_FS_CODES))
    fine = 11 * dac - hi_on - bias_codes
    disturbed = np.zeros(n, dtype=bool)
    disturbed[1:] = (np.diff(dac) != 0) | (np.diff(led_code) != 0)
    frames = np.stack([tia_dark, hi_dark, tia_on, hi_on], axis=1).astype(np.uint16)
    keep = (foot > 0) & (foot < duration_s)
    return PpgSignal(frames, fine, dac.astype(np.uint16), led_code.astype(np.uint16), disturbed,
                     foot[keep], pat[keep])


# ---------------------------------------------------------------- everything

@dataclass
class Simulation:
    ecg_params: EcgParams
    ppg_params: PpgParams
    beats: BeatSchedule
    ecg_codes: np.ndarray     # uint16 at 4 kHz
    ppg: PpgSignal

    def truth(self) -> dict:
        rr = self.beats.rr
        # statistics over normal-to-normal intervals only (ectopic beats excluded)
        nn = rr[~(self.beats.ectopic[1:] | self.beats.ectopic[:-1])] if len(rr) else rr
        h = hrv_stats(nn)
        return {
            "r_peak_times_s": [round(float(x), 6) for x in self.beats.r_times],
            "ectopic": [bool(x) for x in self.beats.ectopic],
            "rr_s": [round(float(x), 6) for x in rr],
            "pulse_foot_times_s": [round(float(x), 6) for x in self.ppg.foot_times],
            "pat_s": [round(float(x), 6) for x in self.ppg.pat_s],
            "hr_bpm": h["hr_bpm"], "sdnn_ms": h["sdnn_ms"], "rmssd_ms": h["rmssd_ms"],
            "hr_bpm_mean_of_all_rr": float(60.0 / np.mean(rr)) if len(rr) else None,
            "pat_mean_s": float(np.mean(self.ppg.pat_s)) if len(self.ppg.pat_s) else None,
        }


def simulate(ecg: EcgParams | None = None, ppg: PpgParams | None = None) -> Simulation:
    ecg = ecg or EcgParams()
    ppg = ppg or PpgParams()
    rng = np.random.default_rng(ecg.seed)
    beats = make_beats(ecg, rng)
    codes = render_ecg(ecg, beats, rng)
    sig = render_ppg(ppg, beats, ecg.duration_s, np.random.default_rng(ppg.seed))
    return Simulation(ecg, ppg, beats, codes, sig)


def schedule_changes(a: np.ndarray) -> list[list[int]]:
    """[[frame, value], ...] at each change of a (including frame 0)."""
    idx = np.concatenate(([0], np.flatnonzero(np.diff(a.astype(np.int64))) + 1))
    return [[int(i), int(a[i])] for i in idx]


def params_dict(sim: Simulation) -> dict:
    d = {"ecg": asdict(sim.ecg_params), "ppg": asdict(sim.ppg_params)}
    d["ecg"]["mains_harmonics"] = list(sim.ecg_params.mains_harmonics)
    d["ppg"]["led_steps"] = [list(x) for x in sim.ppg_params.led_steps]
    return d
