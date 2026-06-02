# tools/

Host-side scripts that don't run on the device. Keep them dependency-light
and runnable straight from a checkout — no virtualenv ceremony.

## `analyze_shots.py`

Descriptive trend analysis of `storage: dump[…]` lines that the device prints
on boot. Independent of the on-device Bayesian model: if the *sign* of any
coefficient here disagrees with what the device is doing, that's a real
divergence worth investigating.

The OLS regresses `log(brew time)`, matching the transform the firmware fits
(`model_math.cpp`), and an out-of-band block replays the device's shot-tip gate
on the logged shots. Both are plain-OLS proxies for the device's ridge+phantom
fit — close in sign and rough magnitude, not bit-exact.

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
- **By temperature bin** — mean grind, sugg, t_s, stars for cool / mid / hot
  bins. Often the most directly readable signal.
- **Correlation matrix** — Pearson r across all numeric fields, restricted
  to rows that have a non-NaN suggestion. The `r(grind, sugg)` cell tells
  you how well the model and your dialing agree.
- **OLS** — `log(actual_time_s) ~ T + H + P + grind`, standardized. See
  "Reading the OLS block" below.
- **Out-of-band check** — replays the device's tip gate on the logged shots.
  See "Reading the out-of-band block" below.
- **Suggestion calibration** — mean / std / max of `(sugg − grind)` over
  shots that had a recorded suggestion.
- **Confidence buckets** — mean `|sugg − grind|` per conf tier, on shots
  where conf was logged (conf-recording is recent — early shots all read 0).

### Reading the OLS block

The fit is on `log(brew time)`, so the response is multiplicative. The header
line reports the **intercept as a time** — `exp(mean log-time)`, the predicted
brew seconds at the climate centroid — with the raw log intercept in
parentheses.

Per feature:

- **`β/1σ`** — standardized coefficient: change in *log* brew time per one
  standard deviation of that feature. Compare magnitudes across features to
  see which carries the most weight; the sign is what to cross-check against
  the device.
- **`practical reading`** — the β linearized back into **seconds at the
  centroid** (`Δt ≈ t_centroid · Δlog t`) in natural units: `per +1 °C`,
  `per +1 %`, `per +1 hPa`, `per +0.05 grind step`. Quote this one to humans.
  Grind uses the dial step (`kGrindStep` from `model_math.hpp`) rather than
  per-unit so the number doesn't extrapolate outside the typically narrow
  dialed range. It's a *local* linearization — accurate near the centroid,
  not for big excursions.

R² is reported in log space (it's the fit's own residual share, not a
seconds-space figure).

At small n (< ~30 shots), trust **signs** more than **magnitudes** —
correlated regressors (climate axes covary, and you dial differently
in different climates) let OLS split credit somewhat arbitrarily. The
device's Bayesian fit handles this better; this tool is the
sanity-check counterpart, not a replacement.

### Reading the out-of-band block

This replays the device's shot-tip decision on your logged shots. It fits the
same log-time OLS, predicts each shot's brew time, and flags the ones the
firmware would tip on — **confident _and_ off-prediction**:

- **gate** — `conf ≥ TIP_CONF_GATE` (80) AND actual/predicted beyond
  `TIP_BAND_RATIO` (1.40×) or its reciprocal (0.71×). Both constants mirror
  `model_math.cpp`; change them there and here together.
- **`log-residual RMS`** — typical miss size, also shown as a ± percentage.
- **flagged table** — `idx`, actual vs predicted seconds, ratio, conf, and
  direction (`long` / `fast`).

Two caveats baked into the footer:

- It's an **in-sample** fit, so a lone outlier partly fits itself — its
  predicted time here is pulled toward the outlier, shrinking the residual.
  The device judges each shot against a model trained *without* it
  (assessment runs before the refit), so it can flag a borderline shot this
  block misses.
- Plain OLS, no ridge/phantoms, so the predicted seconds approximate the
  device's but won't match exactly.

## `svg_to_lvgl.py`

Converts a stroke-style SVG (icons, glyphs) into a header-only C++ file
that draws each subpath as an `lv_line` widget under a transparent root
container. The goal is to ship icons as compile-time
`lv_point_precise_t` arrays without pulling in a full SVG plugin.

Fills, paints, dash patterns, and per-element strokes are intentionally
dropped — the caller passes color and stroke width at draw time, so one
generated icon can re-tint to match the live confidence-tier color
without regenerating.

### Install

```sh
python3 -m pip install --user svgelements
```

### Run

```sh
tools/svg_to_lvgl.py path/to/icon.svg --name coffee_cup \
    --out components/ui/include/icons/coffee_cup.generated.hpp
```

Output paths use the `.generated.<ext>` convention so the file reads as
tool-emitted at a glance in directory listings and `#include` lines —
don't hand-edit, regenerate from the SVG instead.

Flags:

- `--name` — C identifier; the emitted function is `create_<name>`.
- `--tol` — flatten tolerance in SVG units (default `0.5`). Drop to
  `0.25` if a long curve looks faceted on a hero-sized asset.
- `--out` — output `.hpp` path, or `-` for stdout (default).

### Using the output

```cpp
#include "icons/coffee_cup_icon.hpp"

lv_obj_t* icon = svg_icon::create_coffee_cup(parent, kColorAccent);
lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
```

The returned root container is sized to the source viewBox; position it
with `lv_obj_set_pos` / `lv_obj_align` like any other LVGL widget.

### Known gaps

- **Filled shapes**: not supported. When you need fills, the swap-in is
  `lv_vector_path_*` (requires `LV_USE_VECTOR_GRAPHIC`).
- **Pure circles / arcs**: emitted as high-vertex polylines rather than
  `lv_arc`. Cheap to add — detect `Path` containing only `Arc` segments
  with `rx ≈ ry` and emit `lv_arc` widgets.
- **Edge clipping**: root container is the exact viewBox size, so a
  thick stroke at the viewBox edge can clip. Oversize the root by
  `2 * stroke_width` if you hit it.
