#!/usr/bin/env python3
"""Design and verify every fixed-point filter used by fw/dsp (ecg_dsp).

Writes two generated headers (checked in):
  fw/dsp/src/dsp_coefs.h       private coefficient tables (Q15 / Q30)
  fw/dsp/include/dsp_delays.h  public group-delay / timing constants

Usage (from the repository root):
  python3 tools/dsp_design/design_filters.py           # verify specs, write headers
  python3 tools/dsp_design/design_filters.py --check   # verify, fail if headers differ
  python3 tools/dsp_design/design_filters.py --plot DIR  # also save response plots

The output is deterministic: rerunning reproduces the headers byte for byte.
Every spec in the firmware brief is asserted on the QUANTISED coefficients.
"""
import argparse
import os
import sys

import numpy as np
import scipy.signal as ss
from scipy.optimize import brentq

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
COEF_H = os.path.join(ROOT, "fw", "dsp", "src", "dsp_coefs.h")
DELAY_H = os.path.join(ROOT, "fw", "dsp", "include", "dsp_delays.h")
CMD = "python3 tools/dsp_design/design_filters.py"

FS_RAW = 4000.0
DECIM = 8
FS_ECG = FS_RAW / DECIM          # 500 Hz
FS_PPG = 100.0
ADC_UV_PER_CODE = 805.7
STREAM_SCALE = 8                 # stream units per ADC code
GAINS = (558, 1098)

Q15 = 1 << 15
Q30 = 1 << 30

failures = []
report = []


def check(cond, msg):
    report.append(("PASS " if cond else "FAIL ") + msg)
    if not cond:
        failures.append(msg)


def db(x):
    return 20.0 * np.log10(np.maximum(np.abs(x), 1e-15))


# ---------------------------------------------------------------------------
# Quantisation helpers
# ---------------------------------------------------------------------------
def quant_fir_q15(h):
    """Quantise a symmetric FIR to Q15 with DC gain exactly 1.0 (sum 32768).

    The float design is first normalised to unity DC gain (otherwise the
    rounding correction would add a flat error that ruins the stopband), then
    the residual integer error is put on symmetric tap pairs with the largest
    rounding error."""
    h = np.asarray(h, dtype=float)
    h = h / h.sum()
    x = h * Q15
    hq = np.round(x).astype(np.int64)
    n = len(h)
    c = n // 2
    d = int(Q15 - hq.sum())
    while d != 0:
        s = 1 if d > 0 else -1
        err = (x - hq) * s
        if abs(d) == 1 and n % 2 == 1:
            hq[c] += s
            d -= s
        else:
            i = int(np.argmax(err[:c]))
            hq[i] += s
            hq[n - 1 - i] += s
            d -= 2 * s
    assert hq.sum() == Q15
    assert np.all(hq == hq[::-1])
    return hq


def q30(v):
    return int(np.round(v * Q30))


def biquad_q30(b, a):
    """Return (b0,b1,b2,a1,a2) in Q30 from normalised b, a (a[0] == 1)."""
    b = list(b) + [0.0] * (3 - len(b))
    a = list(a) + [0.0] * (3 - len(a))
    assert abs(a[0] - 1.0) < 1e-12
    c = [q30(b[0]), q30(b[1]), q30(b[2]), q30(a[1]), q30(a[2])]
    for v in c:
        assert -(1 << 31) <= v < (1 << 31), "Q30 overflow"
    return c


def sos_from_q30(cs):
    return np.array([[c[0] / Q30, c[1] / Q30, c[2] / Q30, 1.0, c[3] / Q30, c[4] / Q30]
                     for c in cs])


def resp(sos, f, fs):
    _, h = ss.sosfreqz(sos, worN=np.asarray(f, dtype=float), fs=fs)
    return h


def find_3db(sos, fs, lo, hi, ref=0.0):
    fn = lambda f: db(resp(sos, [f], fs))[0] - (ref - 3.0103)
    return brentq(fn, lo, hi)


def group_delay_sos(sos, f, fs):
    b = np.array([1.0])
    a = np.array([1.0])
    for s in sos:
        b = np.convolve(b, s[:3])
        a = np.convolve(a, s[3:])
    _, gd = ss.group_delay((b, a), w=np.asarray(f, dtype=float), fs=fs)
    return gd


# ---------------------------------------------------------------------------
# 1. ECG decimator 4000 -> 500 Hz
# ---------------------------------------------------------------------------
DEC_N = 95                       # odd: integer group delay (47 raw samples)
DEC_L = 12                       # taps per polyphase branch (95 + 1 zero pad)


def design_decimator():
    h = ss.remez(DEC_N, [0, 150, 250, FS_RAW / 2], [1, 0], weight=[1, 1],
                 fs=FS_RAW, maxiter=1000, grid_density=64)
    hq = quant_fir_q15(h)
    f = np.linspace(0, FS_RAW / 2, 1 << 17)
    _, H = ss.freqz(hq / Q15, worN=f, fs=FS_RAW)
    a = db(H)
    pb = a[f <= 150]
    ripple = pb.max() - pb.min()
    att250 = -a[f >= 250].max()
    att350 = -a[f >= 350].max()
    att1k = -a[f >= 1000].max()
    check(ripple <= 0.1, "decimator passband ripple 0-150 Hz %.3f dB p-p (<= 0.1)" % ripple)
    check(att250 >= 40, "decimator attenuation >= 250 Hz: %.1f dB (>= 40)" % att250)
    check(att350 >= 40, "decimator attenuation >= 350 Hz (aliases into 0-150): %.1f dB" % att350)
    report.append("     decimator attenuation >= 1000 Hz: %.1f dB" % att1k)
    s_abs = int(np.abs(hq).sum())
    check(s_abs * 4095 < (1 << 31), "decimator int32 accumulator headroom (sum|h|=%d)" % s_abs)
    hp = np.zeros(DEC_N + 1, dtype=np.int64)
    hp[:DEC_N] = hq
    # polyphase table: input x[8q+p] adds H[p][j] * x into output y[q+j],
    # with y[m] = sum_k h[k] x[8m+7-k]  =>  H[p][j] = h[8j + 7 - p]
    poly = [[int(hp[8 * j + 7 - p]) for j in range(DEC_L)] for p in range(DECIM)]
    # priming: if every input before block q equals v, output y[q+j] already
    # holds v * sum_{k >= 8j+8} h[k]
    tail = [int(hp[8 * j + 8:].sum()) for j in range(DEC_L)]
    return dict(hq=hq, poly=poly, tail=tail, ripple=ripple, att250=att250,
                att350=att350, att1k=att1k)


# ---------------------------------------------------------------------------
# 2. Display path at 500 Hz: 0.5 Hz HP (1st order), notch 50/60 (Q 30), 40 Hz LP
# ---------------------------------------------------------------------------
NOTCH_Q = 30.0


def design_display():
    b, a = ss.butter(1, 0.5, "high", fs=FS_ECG)
    hp = biquad_q30(b, a)
    notch = {}
    for f0 in (50, 60):
        b, a = ss.iirnotch(f0, NOTCH_Q, fs=FS_ECG)
        c = biquad_q30(b, a)
        c[2] = c[0]        # exact zeros on the unit circle
        notch[f0] = c
    b, a = ss.butter(2, 40.0, "low", fs=FS_ECG)
    lp = biquad_q30(b, a)
    out = dict(hp=hp, notch=notch, lp=lp)
    sos_hp = sos_from_q30([hp])
    sos_lp = sos_from_q30([lp])
    f_hp = find_3db(sos_hp, FS_ECG, 0.05, 5)
    f_lp = find_3db(sos_lp, FS_ECG, 10, 100)
    check(abs(f_hp - 0.5) < 0.02, "display HP -3 dB at %.3f Hz (0.5)" % f_hp)
    check(abs(f_lp - 40) < 0.5, "display LP -3 dB at %.2f Hz (40)" % f_lp)
    for f0, c in notch.items():
        sos = sos_from_q30([c])
        depth = -db(resp(sos, [f0], FS_ECG))[0]
        lo = find_3db(sos, FS_ECG, f0 - 10, f0 - 1e-3)
        hi = find_3db(sos, FS_ECG, f0 + 1e-3, f0 + 10)
        r = np.sqrt(c[4] / Q30)
        tau = 1.0 / (FS_ECG * (1 - r))
        check(depth > 60, "notch %d Hz depth %.0f dB (coefficients)" % (f0, depth))
        report.append("     notch %d Hz: -3 dB %.2f..%.2f Hz (BW %.2f Hz, Q %.1f), "
                      "pole radius %.5f, envelope tau %.0f ms" %
                      (f0, lo, hi, hi - lo, f0 / (hi - lo), r, tau * 1e3))
        out["notch_bw_%d" % f0] = hi - lo
    sos_all = sos_from_q30([hp, notch[50], lp])
    out["gd10"] = group_delay_sos(sos_all, [10.0], FS_ECG)[0]
    out["f_hp"], out["f_lp"] = f_hp, f_lp
    return out


# ---------------------------------------------------------------------------
# 4. QRS detector filters at 500 Hz
#    clean: 4th-order Bessel LP (flat group delay) whose zeros are moved onto
#           the unit circle at 50 and 60 Hz (exact nulls, +1 sample linear-phase
#           delay each). Used for R-peak refinement and as the band-pass input.
#    band-pass: clean -> 2nd-order Butterworth HP -> 2nd-order Butterworth LP,
#           corners solved so the composite -3 dB band is 5..15 Hz.
# ---------------------------------------------------------------------------
CLEAN_FC = 30.0


def design_qrs():
    z, p, k = ss.bessel(4, CLEAN_FC, "low", fs=FS_ECG, output="zpk", norm="mag")
    p = sorted(p[np.imag(p) > 0], key=lambda v: abs(v))
    clean = []
    for pp, f0 in zip(p, (50, 60)):
        w0 = 2 * np.pi * f0 / FS_ECG
        b = np.array([1.0, -2 * np.cos(w0), 1.0])
        a = np.real(np.poly([pp, np.conj(pp)]))
        b = b * a.sum() / b.sum()
        c = biquad_q30(b, a)
        c[2] = c[0]
        clean.append(c)
    sos_clean = sos_from_q30(clean)

    def composite(fh, fl):
        h = biquad_q30(*ss.butter(2, fh, "high", fs=FS_ECG))
        l = biquad_q30(*ss.butter(2, fl, "low", fs=FS_ECG))
        return h, l, np.vstack([sos_clean, sos_from_q30([h, l])])

    def edges(fh, fl):
        _, _, sos = composite(fh, fl)
        f = np.linspace(0.5, 60, 6000)
        a = db(resp(sos, f, FS_ECG))
        i = int(np.argmax(a))
        m = a[i]
        lo = brentq(lambda x: db(resp(sos, [x], FS_ECG))[0] - (m - 3.0103), 0.5, f[i])
        hi = brentq(lambda x: db(resp(sos, [x], FS_ECG))[0] - (m - 3.0103), f[i], 60)
        return lo, hi, f[i], m

    fh, fl = 5.0, 15.0
    for _ in range(30):            # fixed-point iteration on the two corners
        lo, hi, _, _ = edges(fh, fl)
        fh *= 5.0 / lo
        fl *= 15.0 / hi
    fh, fl = round(fh, 2), round(fl, 2)
    hpc, lpc, sos_bp = composite(fh, fl)
    lo, hi, fpk, gpk = edges(fh, fl)
    check(abs(lo - 5) < 0.1 and abs(hi - 15) < 0.2,
          "QRS band-pass -3 dB %.2f..%.2f Hz (5..15), peak %.1f Hz %.1f dB" % (lo, hi, fpk, gpk))
    for f0 in (50, 60):
        att = -db(resp(sos_bp, [f0 - 0.5, f0, f0 + 0.5], FS_ECG))
        check(att.min() > 50, "QRS band-pass at %d+-0.5 Hz: >= %.0f dB" % (f0, att.min()))
    att_clean = -db(resp(sos_clean, [50.0, 60.0], FS_ECG))
    check(att_clean.min() > 60, "clean LP nulls 50/60 Hz: %.0f / %.0f dB" % tuple(att_clean))
    f3c = find_3db(sos_clean, FS_ECG, 2, 49)
    gd = group_delay_sos(sos_clean, [1, 5, 10, 15, 20], FS_ECG)
    report.append("     clean LP -3 dB %.1f Hz; group delay 1/5/10/15/20 Hz = %s samples"
                  % (f3c, " ".join("%.2f" % g for g in gd)))
    check(gd.max() - gd.min() < 0.2, "clean LP group delay flat 1..20 Hz (%.2f samples spread)"
          % (gd.max() - gd.min()))
    # R-peak shift through the clean LP measured on a sampled Gaussian R wave
    shift_clean = peak_shift_digital(sos_clean, sigma=0.010)
    gd_bp = group_delay_sos(sos_from_q30([hpc, lpc]), [10.0], FS_ECG)[0]
    return dict(clean=clean, hp=hpc, lp=lpc, fh=fh, fl=fl, lo=lo, hi=hi, f3c=f3c,
                gd_clean=gd, shift_clean=shift_clean, gd_bp=gd_bp)


def peak_shift_digital(sos, sigma, fs=FS_ECG, n_off=16):
    """Average shift (samples) of the parabolically refined peak of a Gaussian
    pulse filtered by sos, over n_off sub-sample positions (what the C code
    does when it refines an R-peak in the clean signal)."""
    shifts = []
    for i in range(n_off):
        t0 = 200 + i / n_off
        n = np.arange(400)
        x = np.exp(-0.5 * ((n - t0) / (sigma * fs)) ** 2)
        y = ss.sosfilt(sos, x)
        k = int(np.argmax(y))
        ym, y0, yp = y[k - 1], y[k], y[k + 1]
        frac = 0.5 * (ym - yp) / (ym - 2 * y0 + yp)
        shifts.append(k + frac - t0)
    return float(np.mean(shifts))


# ---------------------------------------------------------------------------
# Analog ECG chain: peak shift of a Gaussian R wave through the front end
# (U5 AC coupling 0.32 Hz, C34/R36 0.48 Hz HP, U7A 159 Hz LP, R34/C36 339 Hz LP)
# ---------------------------------------------------------------------------
def analog_ecg_shift(sigma=0.010):
    fs = 200000.0
    n = 1 << 20
    t = (np.arange(n) - n // 2) / fs
    x = np.exp(-0.5 * (t / sigma) ** 2)
    f = np.fft.rfftfreq(n, 1 / fs)
    s = 2j * np.pi * f
    H = np.ones_like(s)
    for fc in (0.32, 0.48):
        H *= s / (s + 2 * np.pi * fc)
    for fc in (159.0, 339.0):
        H *= 1 / (1 + s / (2 * np.pi * fc))
    y = np.fft.irfft(np.fft.rfft(x) * H, n)
    k = int(np.argmax(y))
    ym, y0, yp = y[k - 1], y[k], y[k + 1]
    frac = 0.5 * (ym - yp) / (ym - 2 * y0 + yp)
    return (k + frac - n // 2) / fs


# ---------------------------------------------------------------------------
# 10. PPG at 100 Hz: linear-phase FIR LP, -3 dB ~11 Hz (pulse detection and
#     foot timing: exact integer delay, little upstroke smoothing), then for
#     the display only a 1st-order 0.5 Hz HP and a 2nd-order 8 Hz LP
#     (0.5-8 Hz display band).
# ---------------------------------------------------------------------------
PPG_FIR_N = 21
PPG_DARK_N = 15                  # centred tia_dark mean (C: DSP_PPG_DARK_N)


def design_ppg():
    h = ss.firls(PPG_FIR_N, [0, 8, 18, FS_PPG / 2], [1, 1, 0, 0], weight=[1, 4], fs=FS_PPG)
    hq = quant_fir_q15(h)
    f = np.linspace(0, FS_PPG / 2, 1 << 14)
    _, H = ss.freqz(hq / Q15, worN=f, fs=FS_PPG)
    a = db(H)
    f3 = f[np.argmax(a < -3.0103)]
    att = -a[f >= 20].max()
    report.append("     PPG FIR LP: %d taps, -3 dB %.2f Hz, >= %.1f dB from 20 Hz, delay %d frames"
                  % (PPG_FIR_N, f3, att, PPG_FIR_N // 2))
    check(10.0 <= f3 <= 13.0 and att >= 30, "PPG FIR LP corner %.2f Hz (~11), stopband %.1f dB" % (f3, att))
    b, a2 = ss.butter(1, 0.5, "high", fs=FS_PPG)
    hp = biquad_q30(b, a2)
    f_hp = find_3db(sos_from_q30([hp]), FS_PPG, 0.05, 5)
    check(abs(f_hp - 0.5) < 0.02, "PPG HP -3 dB %.3f Hz" % f_hp)
    lp = biquad_q30(*ss.butter(2, 8.0, "low", fs=FS_PPG))
    f = np.linspace(0.05, 49, 1 << 14)
    _, H = ss.freqz(hq / Q15, worN=f, fs=FS_PPG)
    H = H * resp(sos_from_q30([hp, lp]), f, FS_PPG)
    a = db(H)
    i = int(np.argmax(a))
    band = f[a >= a[i] - 3.0103]
    check(0.4 <= band.min() <= 0.6 and 7.0 <= band.max() <= 8.5,
          "PPG display band-pass -3 dB %.2f..%.2f Hz (~0.5-8)" % (band.min(), band.max()))
    s_abs = int(np.abs(hq).sum())
    return dict(hq=hq, hp=hp, lp=lp, f3=f3, att=att, f_hp=f_hp, s_abs=s_abs,
                band=(band.min(), band.max()))


# ---------------------------------------------------------------------------
# 3. Mains Goertzel at 500 Hz, N = 250 (2 Hz bins: 50 and 60 Hz exact)
# ---------------------------------------------------------------------------
GOERTZEL_N = 250


def design_mains():
    c = {f: q30(2 * np.cos(2 * np.pi * f / FS_ECG)) for f in (50, 60)}
    # leakage check: the 60 Hz bin must not see a 50 Hz tone and vice versa
    n = np.arange(GOERTZEL_N)
    for f, g in ((50, 60), (60, 50)):
        x = np.sin(2 * np.pi * f * n / FS_ECG + 0.3)
        X = abs(np.sum(x * np.exp(-2j * np.pi * g * n / FS_ECG)))
        check(X < 1e-9 * GOERTZEL_N, "Goertzel %d Hz bin rejects %d Hz tone" % (g, f))
    return c


def uv_constants():
    k = {}
    for g in GAINS:
        uv = ADC_UV_PER_CODE / (STREAM_SCALE * g)   # uV per stream unit
        k[g] = dict(uv_q20=int(round(uv * (1 << 20))),
                    pp_q28=int(round(uv * 4.0 / GOERTZEL_N * (1 << 28))),
                    uv=uv)
    return k


# ---------------------------------------------------------------------------
# Header generation
# ---------------------------------------------------------------------------
def fmt_list(vals, per_line=8, indent="    "):
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append(indent + ", ".join("%d" % v for v in vals[i:i + per_line]))
    return ",\n".join(lines)


def bq_init(c):
    return "{ %d, %d, %d, %d, %d }" % tuple(c)


def gen_coefs(dec, disp, qrs, ppg, mains, uvk):
    o = []
    o.append("/* GENERATED by %s -- do not edit.\n"
             " * Rerun the command to regenerate; --check verifies this file is current.\n"
             " * Private to fw/dsp: each table macro is expanded in exactly one .c file.\n"
             " * Biquads: { b0, b1, b2, a1, a2 } in Q30, y = b.x - a.y (a0 = 1). */" % CMD)
    o.append("#ifndef DSP_COEFS_H\n#define DSP_COEFS_H\n")
    o.append("/* ECG decimator: %d-tap equiripple FIR, Q15, DC gain exactly 32768.\n"
             " * Passband ripple %.3f dB p-p (0-150 Hz); attenuation %.1f dB >= 250 Hz,\n"
             " * %.1f dB >= 350 Hz, %.1f dB >= 1 kHz. Polyphase layout\n"
             " * DSP_DECIM_POLY[p][j] = h[8j + 7 - p] (h[95] = 0 pad). */"
             % (DEC_N, dec["ripple"], dec["att250"], dec["att350"], dec["att1k"]))
    o.append("#define DSP_DECIM_TAPS %d" % DEC_N)
    o.append("#define DSP_DECIM_L %d" % DEC_L)
    rows = ",\n".join("    { " + ", ".join("%d" % v for v in r) + " }" for r in dec["poly"])
    o.append("#define DSP_DECIM_POLY_INIT { \\\n" + rows.replace("\n", " \\\n") + " \\\n}")
    o.append("/* Priming: output q+j already holds v * DSP_DECIM_TAIL[j] when all past\n"
             " * inputs equal v. */")
    o.append("#define DSP_DECIM_TAIL_INIT { %s }\n" % ", ".join("%d" % v for v in dec["tail"]))

    o.append("/* Display path, 500 Hz */")
    o.append("#define DSP_DISP_HP_INIT %s /* 1st-order HP, -3 dB %.3f Hz */" % (bq_init(disp["hp"]), disp["f_hp"]))
    o.append("#define DSP_DISP_NOTCH50_INIT %s /* Q %.0f, BW %.2f Hz */" % (bq_init(disp["notch"][50]), NOTCH_Q, disp["notch_bw_50"]))
    o.append("#define DSP_DISP_NOTCH60_INIT %s /* Q %.0f, BW %.2f Hz */" % (bq_init(disp["notch"][60]), NOTCH_Q, disp["notch_bw_60"]))
    o.append("#define DSP_DISP_LP_INIT %s /* 2nd-order Butterworth, -3 dB %.2f Hz */\n" % (bq_init(disp["lp"]), disp["f_lp"]))

    o.append("/* QRS detector, 500 Hz. Clean LP: Bessel-4 poles (fc %.0f Hz) with unit-circle\n"
             " * zeros at 50 and 60 Hz (-3 dB %.1f Hz, flat group delay). Band-pass:\n"
             " * HP %.2f Hz + LP %.2f Hz Butterworth-2 after the clean LP: composite\n"
             " * -3 dB %.2f..%.2f Hz. */" % (CLEAN_FC, qrs["f3c"], qrs["fh"], qrs["fl"], qrs["lo"], qrs["hi"]))
    o.append("#define DSP_QRS_CLEAN_INIT { %s, \\\n    %s }" % (bq_init(qrs["clean"][0]), bq_init(qrs["clean"][1])))
    o.append("#define DSP_QRS_HP_INIT %s" % bq_init(qrs["hp"]))
    o.append("#define DSP_QRS_LP_INIT %s\n" % bq_init(qrs["lp"]))

    o.append("/* PPG, 100 Hz: %d-tap linear-phase FIR LP (Q15, -3 dB %.2f Hz, >= %.1f dB\n"
             " * from 20 Hz) for detection / foot timing; display adds a 1st-order HP\n"
             " * %.3f Hz and a Butterworth-2 LP 8 Hz: display band %.2f..%.2f Hz. */"
             % (PPG_FIR_N, ppg["f3"], ppg["att"], ppg["f_hp"], ppg["band"][0], ppg["band"][1]))
    o.append("#define DSP_PPG_FIR_N %d" % PPG_FIR_N)
    o.append("#define DSP_PPG_FIR_INIT { \\\n" + fmt_list(list(ppg["hq"])).replace("\n", " \\\n") + " \\\n}")
    o.append("#define DSP_PPG_HP_INIT %s" % bq_init(ppg["hp"]))
    o.append("#define DSP_PPG_LP_INIT %s\n" % bq_init(ppg["lp"]))

    o.append("/* Mains Goertzel, 500 Hz, window %d: 2 cos(w) in Q30 */" % GOERTZEL_N)
    o.append("#define DSP_MAINS_N %d" % GOERTZEL_N)
    o.append("#define DSP_MAINS_C50 %d" % mains[50])
    o.append("#define DSP_MAINS_C60 %d\n" % mains[60])

    o.append("/* Microvolts per stream unit = 805.7 / (8 * gain): Q20. Mains uVpp per\n"
             " * Goertzel |X| = 4 * uV / N: Q28. */")
    for g in GAINS:
        o.append("#define DSP_UV_Q20_G%d %d /* %.6f uV */" % (g, uvk[g]["uv_q20"], uvk[g]["uv"]))
        o.append("#define DSP_MAINS_PP_Q28_G%d %d" % (g, uvk[g]["pp_q28"]))
    o.append("\n#endif")
    return "\n".join(o) + "\n"


def gen_delays(dec, disp, qrs, analog_s):
    clean_q8 = int(round(qrs["shift_clean"] * 256))
    stream_t0_us = int(round(((DECIM - 1) - (DEC_N - 1) / 2) * 1e6 / FS_RAW))
    analog_us = int(round(analog_s * 1e6))
    ppg_analog_us = int(round((1e6 * 10e-12 + 499e3 * 22e-12) * 1e6))
    o = []
    o.append("/* GENERATED by %s -- do not edit.\n"
             " *\n"
             " * Group delays and timing constants of the ecg_dsp filter chains, computed\n"
             " * and verified by the design script from the quantised coefficients. */" % CMD)
    o.append("#ifndef DSP_DELAYS_H\n#define DSP_DELAYS_H\n")
    o.append("/* ECG decimator: %d-tap linear-phase FIR at 4 kHz, delay (N-1)/2 raw samples.\n"
             " * Stream sample m is computed when raw sample 8m+7 arrives and represents the\n"
             " * signal at raw time 8m + 7 - %d, i.e. t = m * 2000 us + DSP_ECG_STREAM_T0_US. */"
             % (DEC_N, (DEC_N - 1) // 2))
    o.append("#define DSP_DECIM_DELAY_RAW %d" % ((DEC_N - 1) // 2))
    o.append("#define DSP_ECG_STREAM_T0_US (%d)\n" % stream_t0_us)
    o.append("/* Analog ECG front end (0.32 + 0.48 Hz HP, 159 + 339 Hz LP): shift of a\n"
             " * Gaussian R wave (sigma 10 ms) peak, us. Subtracted from R-peak times. */")
    o.append("#define DSP_ECG_ANALOG_DELAY_US %d\n" % analog_us)
    o.append("/* QRS clean LP: R-peak shift in 1/256 samples at 500 Hz (group delay\n"
             " * %.2f..%.2f samples over 1-20 Hz). The detector subtracts it, so reported\n"
             " * R indices are on the stream timeline. */" % (qrs["gd_clean"].min(), qrs["gd_clean"].max()))
    o.append("#define DSP_QRS_CLEAN_DELAY_Q8 %d" % clean_q8)
    o.append("/* QRS band-pass (after clean LP) group delay at 10 Hz, samples (rounded). */")
    o.append("#define DSP_QRS_BP_DELAY %d\n" % int(round(qrs["gd_bp"])))
    o.append("/* Display path group delay at 10 Hz (HP + notch + LP), 1/256 samples. */")
    o.append("#define DSP_DISP_DELAY_Q8 %d\n" % int(round(disp["gd10"] * 256)))
    o.append("/* PPG: linear-phase FIR delay and centred dark-mean delay in frames (both\n"
             " * removed from reported foot times), analog TIA + second stage delay\n"
             " * (1 Mohm*10 pF + 499 kohm*22 pF), us. */")
    o.append("#define DSP_PPG_LP_DELAY %d" % (PPG_FIR_N // 2))
    o.append("#define DSP_PPG_DARK_DELAY %d" % (PPG_DARK_N // 2))
    o.append("#define DSP_PPG_FOOT_DELAY (DSP_PPG_LP_DELAY + DSP_PPG_DARK_DELAY)")
    o.append("#define DSP_PPG_ANALOG_DELAY_US %d" % ppg_analog_us)
    o.append("\n#endif")
    return "\n".join(o) + "\n", dict(clean_q8=clean_q8, stream_t0_us=stream_t0_us,
                                     analog_us=analog_us, ppg_analog_us=ppg_analog_us)


def plots(outdir, dec, disp, qrs, ppg):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    os.makedirs(outdir, exist_ok=True)
    fig, ax = plt.subplots(2, 2, figsize=(12, 8))
    f = np.linspace(0, 2000, 8192)
    _, H = ss.freqz(dec["hq"] / Q15, worN=f, fs=FS_RAW)
    ax[0, 0].plot(f, db(H)); ax[0, 0].set_title("Decimator (Q15)"); ax[0, 0].set_ylim(-80, 5)
    f = np.linspace(0.05, 250, 8192)
    sos = sos_from_q30([disp["hp"], disp["notch"][50], disp["lp"]])
    ax[0, 1].semilogx(f, db(resp(sos, f, FS_ECG))); ax[0, 1].set_title("Display (50 Hz notch)"); ax[0, 1].set_ylim(-60, 5)
    sos = np.vstack([sos_from_q30(qrs["clean"]), sos_from_q30([qrs["hp"], qrs["lp"]])])
    ax[1, 0].plot(f, db(resp(sos, f, FS_ECG)), label="QRS BP")
    ax[1, 0].plot(f, db(resp(sos_from_q30(qrs["clean"]), f, FS_ECG)), label="clean LP")
    ax[1, 0].legend(); ax[1, 0].set_ylim(-80, 5)
    f = np.linspace(0.05, 50, 4096)
    _, H = ss.freqz(ppg["hq"] / Q15, worN=f, fs=FS_PPG)
    H = H * resp(sos_from_q30([ppg["hp"], ppg["lp"]]), f, FS_PPG)
    ax[1, 1].semilogx(f, db(H)); ax[1, 1].set_title("PPG band-pass"); ax[1, 1].set_ylim(-60, 5)
    for a in ax.flat:
        a.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "dsp_responses.png"), dpi=110)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="do not write; fail if headers differ")
    ap.add_argument("--plot", metavar="DIR", help="save response plots (needs matplotlib)")
    args = ap.parse_args()

    dec = design_decimator()
    disp = design_display()
    qrs = design_qrs()
    ppg = design_ppg()
    mains = design_mains()
    uvk = uv_constants()
    analog_s = analog_ecg_shift()
    coefs = gen_coefs(dec, disp, qrs, ppg, mains, uvk)
    delays, dv = gen_delays(dec, disp, qrs, analog_s)

    report.append("     delays: decimator %d raw samples (stream t0 %d us), analog ECG %d us, "
                  "clean LP %.3f samples (%d/256), QRS BP %.2f samples, display %.2f samples @10 Hz, "
                  "PPG FIR %d + dark mean %d frames, PPG analog %d us"
                  % ((DEC_N - 1) // 2, dv["stream_t0_us"], dv["analog_us"], qrs["shift_clean"],
                     dv["clean_q8"], qrs["gd_bp"], disp["gd10"], PPG_FIR_N // 2, PPG_DARK_N // 2, dv["ppg_analog_us"]))
    print("\n".join(report))

    if args.plot:
        plots(args.plot, dec, disp, qrs, ppg)

    stale = []
    for path, text in ((COEF_H, coefs), (DELAY_H, delays)):
        old = open(path).read() if os.path.exists(path) else None
        if old != text:
            stale.append(os.path.relpath(path, ROOT))
            if not args.check:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w") as fh:
                    fh.write(text)
    if failures:
        print("%d spec failure(s)" % len(failures))
        return 1
    if args.check and stale:
        print("stale generated header(s): " + ", ".join(stale))
        return 1
    print("headers %s" % ("up to date" if not stale else "written: " + ", ".join(stale)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
