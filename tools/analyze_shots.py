#!/usr/bin/env python3
"""Descriptive analysis of espressopost shot-log dump[] lines.

Independent of the on-device Bayesian model — won't reproduce its coefficients,
but if the sign of any OLS β here disagrees with what the device emits, that's
worth investigating. The OLS regresses log(brew time), matching the transform
the firmware fits (model_math.cpp), and the out-of-band block replays the
device's tip gate on the logged shots as a proxy (plain OLS, no ridge/phantoms).

Usage:
    tools/analyze_shots.py path/to/monitor.log
    idf.py monitor | tools/analyze_shots.py
"""

import argparse
import datetime
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass

import numpy as np

TOMBSTONE = 0x01
ANOMALY = 0x02

# Mirror the firmware's model constants (model_math.cpp) so the OLS below
# regresses the same transform the device fits and the out-of-band check uses
# the same tip gate. The fit here is plain OLS (no ridge/phantoms), so it's a
# proxy for the device's Bayesian fit — signs + rough magnitudes agree, exact
# coefficients won't.
MIN_BREW_S_FOR_LOG = 1.0  # kMinBrewSecondsForLog — log() floor for stray 0s records
TIP_CONF_GATE = 80        # kTipConfidenceGate — min confidence for a tip to fire
TIP_BAND_RATIO = 1.4      # kTipBandRatio — flag beyond this multiplicative miss

# `taste=0x..` is v4+; v3 dump lines omit it entirely. Keep the group optional
# so a single regex parses both — when absent, taste_flags reads 0 ("none
# reported"), which matches the on-device backward-compat behavior. v5 dump
# lines replace the signed `t_d=<delta>` field with an unsigned `t_s=<actual>`
# raw brew time; only v5 dumps reach this script in practice (the on-device
# migration rewrites older records on first boot), so the regex matches v5.
DUMP_RE = re.compile(
    r"dump\[(?P<idx>\d+)\]:\s*"
    r"ver=(?P<ver>\d+)\s+"
    r"preset=(?P<preset>\d+)\s+"
    r"t_s=(?P<ts>\d+)\s+"
    r"stars=(?P<stars>\d+)\s+"
    r"flags=0x(?P<flags>[0-9a-fA-F]+)\s+"
    r"T=(?P<T>-?[0-9.]+|nan)\s+"
    r"H=(?P<H>-?[0-9.]+|nan)\s+"
    r"P=(?P<P>-?[0-9.]+|nan)\s+"
    r"grind=(?P<grind>-?[0-9.]+|nan)\s+"
    r"sugg=(?P<sugg>-?[0-9.]+|nan)\s+"
    r"conf=(?P<conf>\d+)\s+"
    r"rtc=(?P<rtc>\d+)"
    r"(?:\s+taste=0x(?P<taste>[0-9a-fA-F]+))?"
)


@dataclass
class Shot:
    idx: int
    preset: int
    ts: int   # actual brew time in seconds (v5+)
    stars: int
    flags: int
    T: float
    H: float
    P: float
    grind: float
    sugg: float
    conf: int
    rtc: int
    taste: int

    @property
    def excluded(self) -> bool:
        return bool(self.flags & (TOMBSTONE | ANOMALY))


def _f(s: str) -> float:
    return float("nan") if s == "nan" else float(s)


def parse(stream):
    shots = []
    for line in stream:
        m = DUMP_RE.search(line)
        if not m:
            continue
        shots.append(Shot(
            idx=int(m["idx"]),
            preset=int(m["preset"]),
            ts=int(m["ts"]),
            stars=int(m["stars"]),
            flags=int(m["flags"], 16),
            T=_f(m["T"]), H=_f(m["H"]), P=_f(m["P"]),
            grind=_f(m["grind"]), sugg=_f(m["sugg"]),
            conf=int(m["conf"]),
            rtc=int(m["rtc"]),
            taste=int(m["taste"], 16) if m["taste"] else 0,
        ))
    return shots


def summary_block(shots):
    T = np.array([s.T for s in shots])
    H = np.array([s.H for s in shots])
    P = np.array([s.P for s in shots])
    g = np.array([s.grind for s in shots])
    print(f"{len(shots)} shots")
    print("Climate spread:")
    for name, arr, unit in (("T", T, "°C"), ("H", H, "%"), ("P", P, "hPa")):
        print(f"  {name:<2}: {arr.min():7.2f} – {arr.max():7.2f} {unit:<3}  (mean {arr.mean():6.2f}, std {arr.std(ddof=1):5.2f})")
    print(f"Grind dialed:  {g.min():.2f} – {g.max():.2f}  (mean {g.mean():.2f}, std {g.std(ddof=1):.2f})")


def temp_bin_block(shots):
    bins = [
        ("cool (<22°C)",   lambda s: s.T < 22),
        ("mid  (22-25°C)", lambda s: 22 <= s.T < 25),
        ("hot  (≥25°C)",   lambda s: s.T >= 25),
    ]
    print(f"{'bin':<16} {'n':>3} {'grind':>6} {'sugg':>6} {'t_s':>6} {'stars':>6}")
    for label, pred in bins:
        sub = [s for s in shots if pred(s)]
        if not sub:
            continue
        gs = np.array([s.grind for s in sub])
        sugs = np.array([s.sugg for s in sub if not math.isnan(s.sugg)])
        tss = np.array([s.ts for s in sub])
        st = np.array([s.stars for s in sub])
        sug_s = f"{sugs.mean():>6.2f}" if len(sugs) else "  n/a "
        print(f"{label:<16} {len(sub):>3} {gs.mean():>6.2f} {sug_s} {tss.mean():>6.1f} {st.mean():>6.1f}")


def corr_block(shots):
    cols = ["T", "H", "P", "grind", "sugg", "t_s", "stars"]
    rows = []
    for s in shots:
        row = [s.T, s.H, s.P, s.grind, s.sugg, s.ts, s.stars]
        if any(math.isnan(x) for x in row):
            continue
        rows.append(row)
    if len(rows) < 3:
        print(f"(skipping — only {len(rows)} non-NaN rows)")
        return
    M = np.array(rows).T
    C = np.corrcoef(M)
    print(" " * 7 + "  ".join(f"{c:>6}" for c in cols))
    for i, c in enumerate(cols):
        print(f"{c:<6} " + "  ".join(f"{C[i, j]:>+6.2f}" for j in range(len(cols))))
    print(f"(n={len(rows)})")


def _logtime_fit(shots):
    """Standardized OLS of log(brew seconds) on [T, H, P, grind] — same response
    transform the firmware regresses (model_math.cpp). Returns a dict with beta,
    raw feature std, per-shot prediction (in log space), and the rows used, or
    None if there are too few rows. Plain OLS (no ridge / no phantoms), so it's a
    proxy for the device's Bayesian fit, not a bit-exact reproduction."""
    rows = [s for s in shots
            if not math.isnan(s.T) and not math.isnan(s.grind)]
    if len(rows) < 5:
        return None
    X = np.array([[s.T, s.H, s.P, s.grind] for s in rows], dtype=float)
    y = np.log(np.maximum([s.ts for s in rows], MIN_BREW_S_FOR_LOG))
    std_X = X.std(0, ddof=1)
    std_X_safe = np.where(std_X == 0, 1.0, std_X)
    Xs = (X - X.mean(0)) / std_X_safe
    A = np.hstack([np.ones((len(Xs), 1)), Xs])
    beta, *_ = np.linalg.lstsq(A, y, rcond=None)
    return {"beta": beta, "std_X": std_X, "rows": rows, "y": y, "yhat": A @ beta}


def ols_block(shots):
    fit = _logtime_fit(shots)
    if fit is None:
        n = sum(1 for s in shots
                if not math.isnan(s.T) and not math.isnan(s.grind))
        print(f"(skipping — only {n} rows, need ≥5)")
        return
    beta, std_X, y, yhat = fit["beta"], fit["std_X"], fit["y"], fit["yhat"]
    ss_res = ((y - yhat) ** 2).sum()
    ss_tot = ((y - y.mean()) ** 2).sum()
    r2 = 1 - ss_res / ss_tot if ss_tot else float("nan")
    # intercept is the mean of log-time → exp() is the centroid brew time in
    # seconds. We also use it to linearize the standardized βs back into a
    # seconds-per-step read (Δt ≈ t_centroid · Δlog t), so the practical column
    # stays intuitive even though the fit itself is multiplicative.
    centroid_s = math.exp(beta[0])

    # 0.05 mirrors kGrindStep in model_math.hpp — the dial increment the user
    # actually turns. For grind, "per 1 unit" extrapolates wildly outside the
    # observed range; "per step" is the meaningful read.
    features = [
        ("T",     1.0,   "per +1 °C"),
        ("H",     1.0,   "per +1 %"),
        ("P",     1.0,   "per +1 hPa"),
        ("grind", 0.05,  "per +0.05 step"),
    ]
    print(f"  intercept (predicted brew time at climate centroid):  {centroid_s:.1f} s  (log {beta[0]:+.2f})")
    print(f"  {'feature':<7} {'std':>6} {'β/1σ':>7}   {'practical reading':<20}")
    for (name, step, descr), b_std, sx in zip(features, beta[1:], std_X):
        if sx == 0:
            print(f"  {name:<7} {sx:>6.2f} {b_std:>+7.2f}   (zero variance — skipped)")
            continue
        # Δlog t per step → Δseconds at the centroid time (local linearization).
        b_per_step_s = centroid_s * (b_std / sx) * step
        print(f"  {name:<7} {sx:>6.2f} {b_std:>+7.2f}   {descr:<14} → {b_per_step_s:+6.2f} s")
    print(f"  R² (log space): {r2:.2f}   n={len(fit['rows'])}")
    print("  (signs more reliable than magnitudes at small n; correlated regressors mean OLS splits credit roughly. seconds read is a local linearization at the centroid; the fit is multiplicative.)")


def out_of_band_block(shots):
    """Reproduce the device's out-of-band tip decision on the logged shots,
    using the log-time OLS proxy above. Flags shots that were confident
    (conf ≥ TIP_CONF_GATE) yet missed the prediction by more than the
    multiplicative band — i.e. the ones the firmware would have tipped on."""
    fit = _logtime_fit(shots)
    if fit is None:
        print("(skipping — need ≥5 rows for a fit)")
        return
    rows, y, yhat = fit["rows"], fit["y"], fit["yhat"]
    pred_s = np.exp(yhat)
    actual_s = np.array([s.ts for s in rows], dtype=float)
    log_resid = y - yhat
    band = math.log(TIP_BAND_RATIO)
    rms = math.sqrt((log_resid ** 2).mean())
    print(f"  log-residual RMS: {rms:.3f}  (≈ ±{(math.exp(rms) - 1) * 100:.0f}% typical miss)")
    print(f"  tip gate: conf ≥ {TIP_CONF_GATE} AND actual/predicted beyond "
          f"{TIP_BAND_RATIO:.2f}× (or {1 / TIP_BAND_RATIO:.2f}×)")
    flagged = [(s, actual_s[i], pred_s[i])
               for i, s in enumerate(rows)
               if s.conf >= TIP_CONF_GATE and abs(log_resid[i]) > band]
    if not flagged:
        print("  shots the device would flag out-of-band:  none")
    else:
        print(f"  shots the device would flag out-of-band:  {len(flagged)}")
        print(f"    {'idx':>4} {'actual':>7} {'pred':>7} {'ratio':>6} {'conf':>5}  dir")
        for s, a, p in flagged:
            ratio = a / p
            print(f"    {s.idx:>4} {a:>6.0f}s {p:>6.1f}s {ratio:>5.2f}x {s.conf:>5}  "
                  f"{'long' if ratio > 1 else 'fast'}")
    print("  (in-sample fit, so a lone outlier is partly self-fit — the device judges each shot")
    print("   against the model trained WITHOUT it, so it may flag a borderline shot this misses.)")


def changepoint_block(shots, cut_epoch):
    """Test whether shots after a known event behave like the pre-event model
    predicts. Use this when a discrete physical change — a grinder dial
    recalibration, a burr swap, a fresh bag of beans — may have shifted what the
    logged numbers *mean*, so pooling across it smears two regimes into one.

    Unlike recency-decay weighting (which blurs across the break), this cuts at
    the date: it fits log(time) ~ T + H + P + grind on the pre-cut shots only,
    then reports each post-cut shot's residual against that model. A consistent
    one-sided residual is the fingerprint of a calibration shift — the post-cut
    shots all run long (or all short) for the same dialed grind at matched
    climate. Shots with rtc==0 (RTC never seeded) can't be placed on the
    timeline and are dropped from this view only."""
    dated = [s for s in shots if s.rtc > 0 and not math.isnan(s.grind)]
    before = [s for s in dated if s.rtc < cut_epoch]
    after = [s for s in dated if s.rtc >= cut_epoch]
    if len(before) < 5 or not after:
        print(f"  (need ≥5 pre-cut and ≥1 post-cut dated shots; "
              f"have {len(before)} / {len(after)})")
        return
    Xb = np.array([[1.0, s.T, s.H, s.P, s.grind] for s in before])
    yb = np.log(np.maximum([s.ts for s in before], MIN_BREW_S_FOR_LOG))
    bb, *_ = np.linalg.lstsq(Xb, yb, rcond=None)
    print(f"  pre-cut model trained on {len(before)} shots; "
          f"{len(after)} post-cut shots tested")
    print(f"    {'idx':>4} {'t_s':>5} {'T':>6} {'grind':>6} {'pred':>6} {'resid':>7}")
    resid = []
    for s in after:
        pred = math.exp(bb @ np.array([1.0, s.T, s.H, s.P, s.grind]))
        r = s.ts / pred - 1.0
        resid.append(r)
        print(f"    {s.idx:>4} {s.ts:>5} {s.T:>6.1f} {s.grind:>6.2f} "
              f"{pred:>6.1f} {r * 100:>+6.0f}%")
    resid = np.array(resid)
    n_long = int((resid > 0).sum())
    print(f"  median post-cut residual: {np.median(resid) * 100:+.0f}%   "
          f"({n_long}/{len(resid)} run long)")
    # Two-tailed sign test: P(all n on one side) = 2·0.5^n. All-one-sided at
    # n≥4 (p ≤ 0.125) is the tell that the shift is real, not noise — model the
    # two sides as separate calibration epochs rather than pooling them.
    one_sided = max(n_long, len(resid) - n_long)
    if one_sided == len(resid) and len(resid) >= 4:
        p = 2 * 0.5 ** len(resid)
        print(f"  → all {len(resid)} post-cut shots fall the same side "
              f"(sign test p≈{p:.3f}): looks like a real changepoint. Treat "
              f"pre/post as separate calibration epochs, not one pooled axis.")


def sugg_calib_block(shots):
    deltas = np.array([s.sugg - s.grind for s in shots if not math.isnan(s.sugg)])
    if len(deltas) == 0:
        print("(no shots with a recorded suggestion)")
        return
    print(f"  shots with sugg recorded:  {len(deltas)}")
    print(f"  mean (sugg − grind):       {deltas.mean():+.3f}")
    print(f"  std  (sugg − grind):       {deltas.std(ddof=1):.3f}")
    print(f"  max |sugg − grind|:        {np.abs(deltas).max():.3f}")


def conf_calib_block(shots):
    sub = [s for s in shots if s.conf > 0 and not math.isnan(s.sugg)]
    if not sub:
        print("(no shots with conf>0 — feature was added recently, only newer shots carry it)")
        return
    buckets = [
        ("conf 1-29",   1, 30),
        ("conf 30-59",  30, 60),
        ("conf 60-79",  60, 80),
        ("conf 80-100", 80, 101),
    ]
    print(f"{'bucket':<14} {'n':>3} {'mean |sugg-grind|':>20}")
    for label, lo, hi in buckets:
        rows = [s for s in sub if lo <= s.conf < hi]
        if not rows:
            continue
        ad = np.mean([abs(s.sugg - s.grind) for s in rows])
        print(f"{label:<14} {len(rows):>3} {ad:>20.3f}")


def report(shots, cut_epoch=None):
    print("=== espressopost shot log analysis ===\n")
    kept = [s for s in shots if not s.excluded]
    if len(kept) < len(shots):
        print(f"({len(shots) - len(kept)} shot(s) excluded by tombstone/anomaly flag)\n")
    by_preset = defaultdict(list)
    for s in kept:
        by_preset[s.preset].append(s)
    for preset_id in sorted(by_preset):
        ps = by_preset[preset_id]
        print(f"--- preset {preset_id}  ({len(ps)} shots) ---\n")
        summary_block(ps)
        print("\n[by temperature bin]")
        temp_bin_block(ps)
        print("\n[correlation matrix (Pearson r), non-NaN rows only]")
        corr_block(ps)
        print("\n[OLS: log(actual_time_s) ~ T + H + P + grind   (standardized, mirrors firmware)]")
        ols_block(ps)
        print("\n[out-of-band check (reproduces the device tip decision)]")
        out_of_band_block(ps)
        print("\n[suggestion calibration]")
        sugg_calib_block(ps)
        print("\n[confidence buckets]")
        conf_calib_block(ps)
        if cut_epoch is not None:
            print("\n[changepoint check (pre-cut model vs post-cut shots)]")
            changepoint_block(ps, cut_epoch)
        print()


def main():
    ap = argparse.ArgumentParser(description="Analyze espressopost dump[] log lines.")
    ap.add_argument("log", nargs="?", help="Path to a monitor log; reads stdin if omitted.")
    ap.add_argument("--changepoint", metavar="DATE",
                    help="Date of a known calibration event (YYYY-MM-DD, UTC) or "
                         "raw rtc epoch seconds. Adds a per-preset block testing "
                         "whether post-event shots run consistently off the "
                         "pre-event model — the fingerprint of a dial/burr/bean "
                         "change the pooled fit would otherwise smear.")
    args = ap.parse_args()

    cut_epoch = None
    if args.changepoint:
        if args.changepoint.isdigit():
            cut_epoch = int(args.changepoint)
        else:
            d = datetime.datetime.strptime(args.changepoint, "%Y-%m-%d")
            cut_epoch = int(d.replace(tzinfo=datetime.timezone.utc).timestamp())

    stream = open(args.log) if args.log else sys.stdin
    shots = parse(stream)
    if not shots:
        print("No dump[] lines found.", file=sys.stderr)
        sys.exit(1)
    report(shots, cut_epoch)


if __name__ == "__main__":
    main()
