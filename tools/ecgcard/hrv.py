"""Heart-rate-variability statistics shared by the generator, references and reports."""
from __future__ import annotations

import numpy as np


def hrv_stats(rr_s: np.ndarray) -> dict[str, float]:
    """HR (from mean RR), SDNN (sample std, ddof=1) and RMSSD, with RR in seconds.

    SDNN and RMSSD are returned in milliseconds. Fewer than 2 intervals give NaN.
    """
    rr = np.asarray(rr_s, dtype=float)
    rr = rr[np.isfinite(rr)]
    nan = float("nan")
    if rr.size == 0:
        return {"hr_bpm": nan, "hr_min": nan, "hr_max": nan, "sdnn_ms": nan, "rmssd_ms": nan}
    ms = rr * 1000.0
    sdnn = float(np.std(ms, ddof=1)) if rr.size > 1 else nan
    rmssd = float(np.sqrt(np.mean(np.diff(ms) ** 2))) if rr.size > 1 else nan
    return {
        "hr_bpm": float(60.0 / np.mean(rr)),
        "hr_min": float(60.0 / np.max(rr)),
        "hr_max": float(60.0 / np.min(rr)),
        "sdnn_ms": sdnn,
        "rmssd_ms": rmssd,
    }
