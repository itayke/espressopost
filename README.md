# espressopost

Standalone ESP32-S3 firmware for an espresso-grind helper. Reads ambient
climate, logs per-shot feedback (time delta + 1–5 quality stars), and
learns a per-preset grind adjustment from local data. Offline-first.

## Status

**Steps 1 – 3 of 10 — bringup + climate + shot logging.** Display +
capacitive touch up under LVGL 9 (round 466 × 466, 2-px-aligned partial
redraws), 1 Hz BME280 read loop feeding a P / H / T status strip, and a
Report screen (time-delta stepper + 1–5 stars + Submit) that appends a
32-byte `ShotRecord` to LittleFS on every save. Single hard-coded preset
(id 0); no model yet; RTC field is still 0 (PCF85063 lands in a later
step — `esp_timer` ticks order shots in the meantime).

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
- **RTC:** PCF85063 (wall-clock time source for shot records)
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

1. Boot log shows: `display: display + LVGL ready (466x466, 50-line partial buffer)`, `touch: CST9217 touch ready`, `storage: littlefs mounted at /littlefs (...KB used, N shots logged)`, and (if a BME280 is wired) `climate: BME280 ready at 0x76 on I2C1 (SDA=17 SCL=18 @ 400000 Hz)`.
2. Screen shows the climate strip at the top, a `-` placeholder for
   time delta in the middle, five gray quality circles below it, and a
   muted **Submit** button at the bottom (disabled until both fields
   are set).
3. Tapping `−` / `+` either side of the delta value sets it to a signed
   seconds reading (range −30 … +30). Tapping a star fills it amber and
   any star to its left.
4. Once both delta and stars are set, Submit lights amber. Tapping it
   appends a 32-byte record to `/littlefs/shots.bin`, briefly shows
   `Saved #N` near the top, and clears the form.
5. Power-cycling and submitting another shot shows the count carrying
   over (`Saved #2`, `#3`, …) — proof that LittleFS is persisting.

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
  add this when we implement sleep/wake).
- AMOLED burn-in mitigation (dim/sleep policy is still an open decision).
- PCF85063 RTC, QMI8658 IMU drivers.
- A real preset system — every shot logs `preset_id = 0`.
- The model — every shot logs `click_delta = 0`.
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
│   ├── app_main.cpp            init display → init touch → show bringup screen
│   ├── board_pins.hpp          verbatim from Waveshare's reference repo
│   ├── idf_component.yml       managed deps: lvgl, CO5300, CST9217
│   └── CMakeLists.txt
└── components/
    ├── display/                CO5300 QSPI + LVGL display binding + LVGL task
    ├── touch/                  CST9217 I²C + LVGL pointer indev
    ├── climate/                BME280 1 Hz sample task on H2 I²C bus
    ├── storage/                LittleFS mount + 32-byte ShotRecord append-log
    └── ui/                     Report screen (delta + stars + Submit)
```

Components for `model/`, `presets/`, `grinder/` are intentionally NOT
scaffolded yet — they'll be added in their respective build-order steps
so the tree only contains live code.
