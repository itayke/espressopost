#!/usr/bin/env python3
"""Descriptive analysis of espressopost shot-log dump[] lines.

Independent of the on-device Bayesian model — won't reproduce its coefficients,
but if the sign of any OLS β here disagrees with what the device emits, that's
worth investigating.

Usage:
    tools/analyze_shots.py path/to/monitor.log
    idf.py monitor | tools/analyze_shots.py
"""

import argparse
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass

import numpy as np

TOMBSTONE = 0x01
ANOMALY = 0x02

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


def ols_block(shots):
    rows = [(s.T, s.H, s.P, s.grind, s.ts) for s in shots if not math.isnan(s.T)]
    if len(rows) < 5:
        print(f"(skipping — only {len(rows)} rows, need ≥5)")
        return
    X = np.array([r[:4] for r in rows], dtype=float)
    y = np.array([r[4] for r in rows], dtype=float)
    std_X = X.std(0, ddof=1)
    std_X_safe = np.where(std_X == 0, 1.0, std_X)
    Xs = (X - X.mean(0)) / std_X_safe
    A = np.hstack([np.ones((len(Xs), 1)), Xs])
    beta, *_ = np.linalg.lstsq(A, y, rcond=None)
    yhat = A @ beta
    ss_res = ((y - yhat) ** 2).sum()
    ss_tot = ((y - y.mean()) ** 2).sum()
    r2 = 1 - ss_res / ss_tot if ss_tot else float("nan")

    # 0.05 mirrors kGrindStep in model_math.hpp — the dial increment the user
    # actually turns. For grind, "per 1 unit" extrapolates wildly outside the
    # observed range; "per step" is the meaningful read.
    features = [
        ("T",     1.0,   "per +1 °C"),
        ("H",     1.0,   "per +1 %"),
        ("P",     1.0,   "per +1 hPa"),
        ("grind", 0.05,  "per +0.05 step"),
    ]
    print(f"  intercept (actual brew seconds at climate centroid):  {beta[0]:+.2f} s")
    print(f"  {'feature':<7} {'std':>6} {'β/1σ':>7}   {'practical reading':<20}")
    for (name, step, descr), b_std, sx in zip(features, beta[1:], std_X):
        if sx == 0:
            print(f"  {name:<7} {sx:>6.2f} {b_std:>+7.2f}   (zero variance — skipped)")
            continue
        b_per_step = (b_std / sx) * step
        print(f"  {name:<7} {sx:>6.2f} {b_std:>+7.2f}   {descr:<14} → {b_per_step:+6.2f} s")
    print(f"  R²: {r2:.2f}   n={len(rows)}")
    print("  (signs more reliable than magnitudes at small n; correlated regressors mean OLS splits credit roughly.)")


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


def report(shots):
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
        print("\n[OLS: actual_time_s ~ T + H + P + grind   (standardized)]")
        ols_block(ps)
        print("\n[suggestion calibration]")
        sugg_calib_block(ps)
        print("\n[confidence buckets]")
        conf_calib_block(ps)
        print()


def main():
    ap = argparse.ArgumentParser(description="Analyze espressopost dump[] log lines.")
    ap.add_argument("log", nargs="?", help="Path to a monitor log; reads stdin if omitted.")
    args = ap.parse_args()

    stream = open(args.log) if args.log else sys.stdin
    shots = parse(stream)
    if not shots:
        print("No dump[] lines found.", file=sys.stderr)
        sys.exit(1)
    report(shots)


if __name__ == "__main__":
    main()
