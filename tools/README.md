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
