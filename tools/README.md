# tools/

Host-side scripts that don't run on the device. Keep them dependency-light
and runnable straight from a checkout — no virtualenv ceremony.

## `analyze_shots.py`

Descriptive trend analysis of `storage: dump[…]` lines that the device prints
on boot. Independent of the on-device Bayesian model: if the *sign* of any
coefficient here disagrees with what the device is doing, that's a real
divergence worth investigating.

### Install

Only depends on numpy:

```sh
python3 -m pip install --user numpy
```

(or use a venv — see the root README's note on macOS Python.)

### Run

```sh
tools/analyze_shots.py path/to/monitor.log     # file arg
idf.py monitor | tools/analyze_shots.py        # stdin
```

It greps `dump[…]` lines itself, so feeding it a full monitor capture is
fine.

### What the output covers

Per preset (one section each):

- **Climate spread** — range, mean, std of T / H / P; range/mean/std of the
  dialed grind. Quick "did I actually pull across enough climate yet?" check.
- **By temperature bin** — mean grind, sugg, t_d, stars for cool / mid / hot
  bins. Often the most directly readable signal.
- **Correlation matrix** — Pearson r across all numeric fields, restricted
  to rows that have a non-NaN suggestion. The `r(grind, sugg)` cell tells
  you how well the model and your dialing agree.
- **OLS** — `time_delta ~ T + H + P + grind`, standardized. See "Reading
  the OLS block" below.
- **Suggestion calibration** — mean / std / max of `(sugg − grind)` over
  shots that had a recorded suggestion.
- **Confidence buckets** — mean `|sugg − grind|` per conf tier, on shots
  where conf was logged (conf-recording is recent — early shots all read 0).

### Reading the OLS block

Two β columns are printed per feature:

- **`β/1σ`** — standardized coefficient: seconds of `time_delta` per one
  standard deviation change in that feature. Compare magnitudes across
  features to see which one carries the most weight in the fit.
- **`practical reading`** — the same β converted to natural units:
  `per +1 °C`, `per +1 %`, `per +1 hPa`, `per +0.05 grind step`. Quote this
  one to humans. Grind uses the dial step (`kGrindStep` from
  `model_math.hpp`) rather than per-unit so the number doesn't extrapolate
  outside the typically narrow dialed range.

At small n (< ~30 shots), trust **signs** more than **magnitudes** —
correlated regressors (climate axes covary, and you dial differently
in different climates) let OLS split credit somewhat arbitrarily. The
device's Bayesian fit handles this better; this tool is the
sanity-check counterpart, not a replacement.
