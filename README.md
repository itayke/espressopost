# espressopost

Standalone ESP32-S3 firmware for an espresso-grind helper. Reads ambient
climate, logs per-shot feedback (raw brew seconds + 1–5 quality stars),
and learns a per-preset grind adjustment from local data. Offline-first.

## Status

**Steps 1 – 5 (model v1) + idle policy + RTC + grind capture + ring UI.**
Display + capacitive touch up under LVGL 9 (round 466 × 466, 2-px-aligned
partial redraws), 1 Hz BME280 read loop, an NVS-backed preset table (3
defaults seeded on first boot, selection persistent across reboots, each
carrying its own accent color), and a three-mode screen built around a
rotating grind bezel: **Idle** shows the selected preset readout, climate
strip, the current grind value big in the center, and **Post** / **Menu**
buttons on the center line; the outer rim is a 0 – 30 ring (0.1 steps)
that rotates under finger drag with a static down-arrow at 6 o'clock
indicating the live dial value. A second arrow on the ring — green,
orange, or red by confidence tier (`>80 / >50 / >30`, hidden below) —
points at the model's recommended grind, so the user can see at a
glance how far they are from it. Tapping **Post** swaps the center
content for a time-delta stepper + 1–5 stars + Submit; the ring stays
live so they can keep dialing. Submit appends a 40-byte v3 `ShotRecord`
to LittleFS and returns to Idle. Tapping **Menu** swaps to the **Presets**
screen — a "PRESETS" title over a 3×3 grid of slots (each showing
`PRESET N` / `Xg → Yg` / `Zs` tinted in the preset's accent color, empty
slots a bare outline) with a Back pill that returns to Idle; the mode
swap fades each section individually rather than the whole screen. The
grid is view-only for now — per-slot select/edit/color-pick is the next
step. The grind value persists per preset
(NVS key `gN`); the model's recommendation lands in a separate
`suggested_grind` field on the record. An idle watchdog dims the AMOLED 
after 30 s and turns the panel off after 2 min; any touch wakes it
(the wake-tap is swallowed for 500 ms so it doesn't accidentally hit a
widget). PCF85063 RTC driven over the shared internal I²C bus: on first
boot (or after the backup cell dies) the chip's OS flag is set and the
driver seeds the clock from `__DATE__`/`__TIME__` (off by the build
machine's TZ, typically <24 h — fine as a floor until a real time-set
flow lands); subsequent boots read the live clock straight off the chip
and `ShotRecord.rtc_epoch_s` is a real wall-clock value.

**Model v1 (time model only):** per-preset Bayesian linear regression
on `actual_time_s ~ grind + T + H + P` with a unit-variance Gaussian
ridge prior (effective ≈ 3 shots per coefficient). Shot records carry the
raw brew time in seconds (v5+) instead of a delta against the preset
target, so retuning a preset's target time only shifts what `suggest()`
aims for — β stays invariant and no history is invalidated. Two synthetic
"phantom" shots get folded into every fit at
`{grind_mean − 0.5, time_mean + 10s}` and
`{grind_mean + 0.5, time_mean − 10s}` (weight 0.5 each), built at fit
time so the directional prior tracks wherever on the dial — and wherever
in the brew-time range — the user actually pulls. They (a) inject grind variance so `std_g` never sits at the
floor while real shots cluster at one dial setting, (b) seed `β_g`
with the right sign (finer→longer) so the first suggestion points the
user the right way, and (c) fade as real data accumulates (two real
shots already out-vote them). Refits on boot and after every submit.
The ring shows a colored arrow at the model's recommended grind whenever
the **combined confidence** clears the `>30` threshold: green above 80,
orange above 50, red above 30, hidden below. Combined confidence is the
average of three 0..1 factors: (1) slope-quality from `Σ[0,0]` (how well
we know the grind→time slope), (2) climate-extrapolation distance (how
far live T/H/P sits outside the trained range, full credit within ±1σ
decaying to 0 by ±3σ), and (3) grind-extrapolation distance (how far the
recommended dial position sits outside the user's observed range, same
shape). Internally still rounded to nearest 5 %, capped at 95 % — the
numeric value is no longer on screen, only in the `state:` debug log.
The cached suggestion at submit time is what gets stored in
`ShotRecord.suggested_grind` (NaN when the arrow was hidden), so each
shot records the model state the user actually saw. Quality model,
cross-preset pooling, and recency-weighted shots from the spec are
deferred — the time-only model gets us a useful directional indicator
while data accumulates, and recency weighting was dropped in v1 because
we have no signal yet that bean-age / grinder drift matters enough to
justify guessing a half-life.

**Shot-record migrations run on first boot of this build:** any v1/v2
records (32 B, no grind data) get rewritten in storage::init() to the
40 B v3 layout with `user_grind = 5.2f` and `suggested_grind = NaN`. A
second pass in `storage::finalize_migrations()` (called after
`presets::init()` so it can look up each shot's `preset.target_time_s`)
converts v3/v4 records — which stored `time_delta_s` — to v5, where the
byte at offset 2 holds the raw `actual_time_s` instead. Both steps go
through a temp file + atomic rename so a power loss mid-migration retries
on the next boot. Preset blobs in NVS get the same family of treatment:
v1 (20 B, int8 `click_anchor`) → v2 (24 B, float `grind_anchor = 5.2f`),
v2 → v3 (last byte repurposed as `yield_g`), v3 → v4 (28 B, appends a
`uint32_t color` accent, seeded `0xE0E0E0`). Size keys the migration path
(20/24/28 B); a version byte disambiguates within a shared size. After first boot logs
`storage: migrated N shots from v2 → v3` /
`storage: migrated N shots from v3/v4 → v5` and the preset migration
line, every step becomes a no-op forever.

The full build order lives in the kickoff brief. Each step ends in a
runnable device state.

## Hardware target

Waveshare ESP32-S3-Touch-AMOLED-1.75 (the round 466 × 466 AMOLED with
case).

- **MCU:** ESP32-S3R8 (8 MB OPI PSRAM, 16 MB QIO flash)
- **Display:** CO5300 AMOLED over QSPI (SPI2_HOST)
- **Touch:** CST9217 over I²C (shared bus with PMIC/RTC/IMU)
- **PMIC:** AXP2101 (display rail gated by PMIC defaults — works at
  power-on without configuration; we'll drive it later for sleep)
- **RTC:** PCF85063 on shared internal I²C bus @ 0x51, battery-backed
  (CR1220) — wall-clock source for `rtc_epoch_s` in every shot record
- **IMU:** QMI8658 (planned wake-on-motion)
- **Audio:** ES7210 dual-mic codec (unused in v1)
- **Climate:** Bosch BME280 (temperature / humidity / pressure) on the
  8-pin H2 header — its own dedicated I²C bus on GPIO17 (SDA) / GPIO18
  (SCL), separate from the internal bus to avoid contention with
  PMIC/RTC/IMU/touch
- **Expansion:** H2 also carries VBUS, 3V3, GND, U0RXD/U0TXD, and one
  free GPIO (16) for future use

Pin map is verbatim from Waveshare's reference: see
[`main/board_pins.hpp`](main/board_pins.hpp).

## Toolchain

- ESP-IDF ≥ v5.5, installed at `~/esp/esp-idf`
- LVGL 9.x, pulled by the component manager
- C++17

### System prerequisites

ESP-IDF expects `cmake` and `ninja` on PATH — it doesn't bundle them. On macOS:

```sh
brew install cmake ninja
```

### One-time IDF install (if `idf.py` isn't on PATH)

```sh
mkdir -p ~/esp && cd ~/esp
git clone --recursive --depth 1 -b v5.5 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3                     # ~10 min, ~3 GB; toolchain → ~/.espressif/
```

`~/.zshrc` defines a `get_idf` alias that puts `idf.py` and the xtensa
toolchain on PATH for the current shell. Open a fresh terminal after
install so the alias is loaded.

## Build

```sh
get_idf                                  # makes idf.py available in this shell
cd /Users/itaykeren/Code/GitHub/espressopost

# First time only:
idf.py set-target esp32s3

# Pull managed components (LVGL, CO5300 driver, CST9217 driver):
idf.py reconfigure

idf.py build
```

If clangd in your editor shows red squigglies on `esp_err_t`, `lv_*`, etc.
before the first build — that's expected; clangd has no project context
until `idf.py reconfigure` generates `build/compile_commands.json`. After
the first reconfigure the editor diagnostics clear. Clangd will still
warn about a few xtensa-gcc-only flags (`-mlongcalls`, `-fstrict-volatile-bitfields`,
etc.); those are cosmetic and don't affect the build — to silence them,
add a `.clangd` file with `CompileFlags: { Remove: [-mlongcalls, ...] }`
when it gets annoying.

### Changing build config after first build

`sdkconfig.defaults` is only consulted when there's no `sdkconfig` to
load from. After the initial build, edits to `sdkconfig.defaults` will
**not** take effect on rebuild. To apply changes:

- Quick: `rm sdkconfig && idf.py build` — regenerates from defaults.
- Interactive: `idf.py menuconfig` — changes write directly to `sdkconfig`.

## Flash & monitor

```sh
# Find the port: ls /dev/cu.usbmodem* (macOS) or /dev/ttyUSB* (Linux).
idf.py -p /dev/cu.usbmodem<N> flash monitor
```

Exit the monitor with `Ctrl+]`.

## Host tests

The model's math (`components/model/model_math.{hpp,cpp}`) is deliberately
free of ESP-IDF includes so it can compile and run on the dev machine — no
flash, no device, no IDF environment. That gives us a millisecond-feedback
loop for tuning priors, weights, and confidence behavior.

```sh
./tests/host/run.sh                       # build + run all cases
./tests/host/run.sh "[confidence]"        # filter by tag
./tests/host/run.sh -s                    # show successful assertions too
```

The first run will configure CMake under `tests/host/build/` (Ninja);
subsequent runs only re-link. Requires `cmake` and `ninja` on PATH (same
prerequisites as the ESP-IDF build).

Tests live in [`tests/host/test_model.cpp`](tests/host/test_model.cpp) and
use Catch2 v2 (single-header, vendored under
[`tests/host/third_party/`](tests/host/third_party/) — no install). The
design rule: if you ever need an IDF include in `model_math.{hpp,cpp}` to
make a test pass, the test is wrong — push the IDF dependency back into
`model.cpp` (the wrapper) and have the test build a synthetic input
instead.

## What to verify on this build

1. Boot log shows: `display: display + LVGL ready (466x466, 50-line partial buffer)`, `touch: CST9217 touch ready`, `storage: littlefs mounted at /littlefs (...KB used, N shots logged)`, `presets: 3 presets loaded, selected=N ("espresso")`, (if a BME280 is wired) `climate: BME280 ready at 0x76 on I2C1 (SDA=17 SCL=18 @ 400000 Hz)`, the RTC line — either `rtc: first boot (OS=1) — seeding from build time …` on a virgin chip, or `rtc: PCF85063 already set: 2026-MM-DD HH:MM:SS UTC (epoch …)` on subsequent boots — and `model: refit: N records on disk, M presets fit, K shots used in fits`.
2. **Idle screen** shows, from the outer rim inward: a 0 – 30 grind ring
   with major-tick numerals (0, 1, …, 30), a static white down-arrow at
   6 o'clock pointing into the ring at the live dial value, and —
   whenever combined confidence exceeds 30 — a second colored arrow on
   the ring pointing at the model's recommended grind (green above 80,
   orange above 50, red above 30). Inside the ring: the active preset
   row (e.g. `espresso · target 30s`, tappable), the climate strip, the
   big grind value (e.g. `5.2`), and an amber **Post** button.
3. Tapping the preset row cycles through the seeded presets (`espresso`
   → `lungo` → `ristretto` → back). Target seconds and the grind value
   both update — each preset carries its own `grind_anchor` (all three
   ship seeded to 5.2 on first boot or after migration) and remembers
   the last grind value you dialed for it (NVS key `gN`, falls back to
   `grind_anchor` until you change anything). The predicted arrow on the
   ring may move or change color as the new preset's model takes over.
4. Drag anywhere on the rim to rotate the ring; values snap to 0.1 and
   the big readout updates live. Direction follows the finger (drag CW
   = dial UP), and the value is clamped to 0.0 … 30.0. The final value
   on release is persisted to NVS so a reboot resumes on the same dial
   setting per preset.
5. Tapping **Post** swaps the center for a brew-time stepper (`-` / `+`
   around a value in seconds, range 0 … 99 — the live delta vs the
   preset's target is shown alongside, derived on the fly), five gray
   quality circles, and a muted **Submit** button (disabled until both
   the time and stars are set). The ring + cursor + predicted arrow
   stay live — if the user forgot to dial before tapping Post, they can
   finish from here. A small `×` near the top cancels back to Idle
   without saving.
6. Once brew time and stars are set, Submit lights amber. Tapping it
   appends a 40-byte v5 record to `/littlefs/shots.bin` (carrying
   `actual_time_s` from the brew-time stepper, `user_grind` from the
   dial, and `suggested_grind` from whatever the predicted arrow was
   pointing at at submit time — NaN when the arrow was hidden), briefly
   shows `Saved #N` near the top, returns to Idle, and clears the
   post-form fields. The model refits immediately after the append, so
   the predicted arrow on the next climate tick reflects the new data
   point.
7. Power-cycling restores the last-selected preset (NVS persists it),
   the last grind value for each preset, and the shot count carries
   over (`Saved #2`, `#3`, …). Older firmware (pre-30.0 cap) that left a
   grind value above 30 in NVS gets clamped on load — the raw NVS bytes
   stay put, but the UI won't show or persist a value outside the ring.
8. Leaving the device untouched for 30 s drops brightness;
   another 90 s and the panel turns off entirely. Touching it
   anywhere brings it back to full brightness, and the first 500 ms of
   touch after wake doesn't register as a tap (so you can pick the
   device up without inadvertently rotating the ring or hitting Post).
9. The predicted arrow should appear on the very first usable shot
   thanks to the phantom prior (two synthetic shots centered on the
   real-shot grind mean: `{real_mean − 0.5, +10s}` and
   `{real_mean + 0.5, −10s}`, injected into every fit). Expect it to
   come up red (low band) for the first few shots and warm to orange /
   green as real data accumulates. The convention encoded in the
   phantoms is "lower grind number = finer = longer shot" — if your
   grinder runs the opposite direction, flip the
   `delta_from_y_center_s` signs in the `phantoms[]` array inside
   `fit()` in
   [`components/model/model_math.cpp`](components/model/model_math.cpp).

If any of these fail, the most likely culprits in order:

- **Black screen, no log:** wrong toolchain (sourced an older IDF), or
  `idf.py set-target esp32s3` was skipped.
- **Display garbled / sheared / wrong colors:** byte-swap, set_gap, or
  even-pixel rounder regressed in `display.cpp` — these CO5300 quirks
  are documented inline at each fix site.
- **Screen shows but no touch / touch is rotated:** verify CST9217
  address `0x5A` and the `mirror_x` / `mirror_y` / `swap_xy` flags in
  `touch.cpp::init_panel` — the panel and the touch controller don't
  agree on orientation by default.
- **`storage: littlefs mount failed`:** usually `partitions.csv` /
  `sdkconfig.defaults` drifted apart, or an in-flight format was
  interrupted. `idf.py erase-flash` + reflash will recover (destroys
  any previously logged shots, of course).
- **`Submit` lights amber but `Saved #N` never appears / `append_shot
  failed`:** check the `storage:` log line; `ESP_FAIL` is usually a
  full or wedged partition. Same recovery as above.
- **`climate: BME280 not found at 0x76 or 0x77` and a silent bus
  scan:** wiring on H2 — most often SDA / SCL swapped, no 3V3, or (on
  bare breakouts) the CSB pin floating instead of tied to VCC, which
  leaves the sensor in SPI mode.
- **`climate: ... 0x?? ACKs` but not at 0x76/0x77:** something other
  than a BME280 is on the bus, or it's a BMP280 (chip id `0x58`,
  pressure + temp only — drop the humidity bits if you want to support
  it).

## What's deliberately NOT here yet

- AXP2101 PMIC driver (board powers up with sensible defaults; we'll
  add this when battery / true light-sleep matters).
- QMI8658 IMU driver — wake-on-motion is a planned upgrade so the
  device can also wake by picking it up, not just touching it.
- A real time-set flow — first-boot seeding from `__DATE__`/`__TIME__`
  is approximate (off by the build machine's TZ; minutes-to-hours stale
  by the time the device actually boots). NTP-over-companion-app, a
  serial command, or a hidden UI gesture will eventually replace it.
- A Preset editor — the Presets screen renders the 3×3 grid but is
  view-only; per-slot select, edit (dose / yield / target time), and
  color-pick are the next UI step. The table itself is still seeded from
  hard-coded defaults.
- The model's full spec — v1 ships a time-only Bayesian regression
  fitted per preset. The quality model (peak-quality grind via
  `α + β·grind + γ·grind² + climate + interactions`) and cross-preset
  pooling of climate slopes are deferred until the per-preset model is
  observed to overfit / underfit on real data.

Each is its own step in the brief's build order. See the project's
memory notes for design decisions already locked in.

## Layout

```
.
├── CMakeLists.txt              top-level project
├── partitions.csv              custom: nvs + factory (4 MB) + littlefs (~12 MB)
├── sdkconfig.defaults          PSRAM, flash, partition table, FreeRTOS tick
├── main/
│   ├── app_main.cpp            display → touch → storage → presets → storage.finalize_migrations → climate → rtc → model → power → ui
│   ├── board_pins.hpp          verbatim from Waveshare's reference repo
│   ├── idf_component.yml       managed deps: lvgl, CO5300, CST9217, littlefs
│   └── CMakeLists.txt
└── components/
    ├── display/                CO5300 QSPI + LVGL display binding + LVGL task
    ├── touch/                  CST9217 I²C + LVGL pointer indev
    ├── climate/                BME280 1 Hz sample task on H2 I²C bus
    ├── storage/                LittleFS mount + 40-byte ShotRecord append-log
    ├── presets/                NVS-backed Preset table + tap-to-cycle selection
    ├── rtc/                    PCF85063 driver: build-time seed + epoch_s() for ShotRecord
    ├── power/                  idle state machine: dim @ 30s, off @ 2min, wake on touch
    ├── model/                  per-preset Bayesian time model + suggested grind + confidence
    │   ├── include/model.hpp      IDF-bound API (init/refit/suggest_for_preset)
    │   ├── include/model_math.hpp pure math API (FitSample, fit, suggest, Suggestion)
    │   ├── model.cpp              IDF glue: mutex, storage, climate, logging
    │   └── model_math.cpp         pure math: standardization, ridge prior, Σ
    └── ui/                     screen build (Idle / Post / Presets modes) + mode registry
        ├── include/ui.hpp              public API (start_report)
        ├── ui_report.cpp              screen build + mode registry (switch_mode) + section-swap engine + refreshers + handlers
        ├── ui_presets.{hpp,cpp}       Presets screen: "PRESETS" title + 3×3 slot grid + Back pill + menu/back glyphs
        ├── ui_preset_readout.{hpp,cpp} shared "PRESET N / Xg→Yg / Zs" readout (idle center line, post surface, grid slots)
        ├── ui_bar.{hpp,cpp}           generic scroll/momentum bar engine (grind dial is the only consumer)
        └── ui_theme.hpp               shared layout frame + base palette (mode-specific tuning stays in ui_report)

tests/
└── host/                          host-side unit tests for model_math (no IDF)
    ├── CMakeLists.txt             vanilla CMake, system compiler
    ├── run.sh                     configure + build + run one-liner
    ├── test_model.cpp             Catch2 cases
    └── third_party/catch.hpp      vendored Catch2 v2 single header

tools/                             host-side scripts (see tools/README.md)
└── analyze_shots.py               descriptive trend analysis of dump[] log lines
```

The `grinder/` component is intentionally NOT scaffolded yet — it'll
be added in its build-order step so the tree only contains live code.
