# espressopost

Standalone ESP32-S3 firmware for an espresso-grind helper. Reads ambient
climate, logs per-shot feedback (time delta + 1–5 quality stars), and
learns a per-preset grind adjustment from local data. Offline-first.

## Status

**Steps 1 – 4 + idle policy + RTC.** Display + capacitive touch up under
LVGL 9 (round 466 × 466, 2-px-aligned partial redraws), 1 Hz BME280 read
loop feeding a P / H / T status strip, an NVS-backed preset table (3
defaults seeded on first boot, selection persistent across reboots), and
a Report screen (tappable preset row at top, time-delta stepper + 1–5
stars + Submit) that appends a 32-byte `ShotRecord` to LittleFS on every
save. An idle watchdog dims the AMOLED to 33 % after 30 s and turns the
panel off after 2 min; any touch wakes it (the wake-tap is swallowed for
500 ms so it doesn't accidentally hit a widget). PCF85063 RTC driven over
the shared internal I²C bus: on first boot (or after the backup cell
dies) the chip's OS flag is set and the driver seeds the clock from
`__DATE__`/`__TIME__` (off by the build machine's TZ, typically <24 h —
fine as a floor until a real time-set flow lands); subsequent boots read
the live clock straight off the chip and `ShotRecord.rtc_epoch_s` is a
real wall-clock value. No model yet (`click_anchor` and `click_delta`
are still 0).

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

## What to verify on this build

1. Boot log shows: `display: display + LVGL ready (466x466, 50-line partial buffer)`, `touch: CST9217 touch ready`, `storage: littlefs mounted at /littlefs (...KB used, N shots logged)`, `presets: 3 presets loaded, selected=N ("espresso")`, (if a BME280 is wired) `climate: BME280 ready at 0x76 on I2C1 (SDA=17 SCL=18 @ 400000 Hz)`, and the RTC line — either `rtc: first boot (OS=1) — seeding from build time …` on a virgin chip, or `rtc: PCF85063 already set: 2026-MM-DD HH:MM:SS UTC (epoch …)` on subsequent boots.
2. Screen shows, top to bottom: a tappable preset row (e.g. `espresso ·
   target 30s`), the climate strip, a `-` placeholder for time delta, a
   `-` / `+` stepper around it, five gray quality circles, and a muted
   **Submit** button (disabled until both fields are set).
3. Tapping the preset row cycles through the seeded presets (`espresso`
   → `lungo` → `ristretto` → back) and the target seconds update with it.
4. Tapping `−` / `+` either side of the delta value sets it to a signed
   seconds reading (range −30 … +30). Tapping a star fills it amber and
   any star to its left.
5. Once both delta and stars are set, Submit lights amber. Tapping it
   appends a 32-byte record to `/littlefs/shots.bin`, briefly shows
   `Saved #N` near the top, and clears the form.
6. Power-cycling restores the last-selected preset (NVS persists it) and
   shot count carries over (`Saved #2`, `#3`, …).
7. Leaving the device untouched for 30 s drops brightness to ~33 %;
   another 90 s and the panel turns off entirely. Touching it
   anywhere brings it back to full brightness, and the first 500 ms of
   touch after wake doesn't register as a tap (so you can pick the
   device up without inadvertently submitting a shot).

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
- A Preset editor — names, target time, dose, and the table itself are
  hard-coded defaults until the UI overhaul.
- The model — every shot logs `click_delta = 0` and `click_anchor = 0`.
- The idle screen (Report is the only screen for now) and any
  read-back / history UI.

Each is its own step in the brief's build order. See the project's
memory notes for design decisions already locked in.

## Layout

```
.
├── CMakeLists.txt              top-level project
├── partitions.csv              custom: nvs + factory (4 MB) + littlefs (~12 MB)
├── sdkconfig.defaults          PSRAM, flash, partition table, FreeRTOS tick
├── main/
│   ├── app_main.cpp            display → touch → storage → presets → climate → rtc → power → ui
│   ├── board_pins.hpp          verbatim from Waveshare's reference repo
│   ├── idf_component.yml       managed deps: lvgl, CO5300, CST9217, littlefs
│   └── CMakeLists.txt
└── components/
    ├── display/                CO5300 QSPI + LVGL display binding + LVGL task
    ├── touch/                  CST9217 I²C + LVGL pointer indev
    ├── climate/                BME280 1 Hz sample task on H2 I²C bus
    ├── storage/                LittleFS mount + 32-byte ShotRecord append-log
    ├── presets/                NVS-backed Preset table + tap-to-cycle selection
    ├── rtc/                    PCF85063 driver: build-time seed + epoch_s() for ShotRecord
    ├── power/                  idle state machine: dim @ 30s, off @ 2min, wake on touch
    └── ui/                     Report screen (preset / delta / stars / Submit)
```

Components for `model/`, `grinder/` are intentionally NOT scaffolded yet
— they'll be added in their respective build-order steps so the tree
only contains live code.
