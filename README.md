# espressopost

Standalone ESP32-S3 firmware for an espresso-grind helper. Reads ambient
climate, logs per-shot feedback (time delta + 1–5 quality stars), and
learns a per-preset grind adjustment from local data. Offline-first.

## Status

**Steps 1 + 2 of 10 — bringup + climate.** Display + capacitive touch
up under LVGL 9 (round 466 × 466, full-rim touch arc, 2-px-aligned
partial redraws), and a 1 Hz BME280 read loop on the H2 expansion
header feeding a P / H / T status strip at the top of the screen. No
storage, presets, or model yet.

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

1. Boot log shows: `display: display + LVGL ready (466x466, 50-line partial buffer)`, `touch: CST9217 touch ready`, and (if a BME280 is wired) `climate: BME280 ready at 0x76 on I2C1 (SDA=17 SCL=18 @ 400000 Hz)`.
2. Screen shows the number **50** centered on a black background, in
   amber.
3. An amber arc traces three-quarters of the rim with a gap at the
   bottom; a knob sits where the arc meets the indicator.
4. A muted gray status strip sits just under the rim near the top,
   reading e.g. `P 30.05inHg  H 42%  T 72.3°F` and updating once per
   second. Without a sensor it stays as `P --  H --  T --` and the
   rest of the app still works.
5. Dragging the knob around the rim with a finger changes the number
   smoothly between 0 and 100, with no tearing or shearing.

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
- LittleFS, shot records, presets.
- The model.
- The Report screen and idle screen.

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
    └── ui/                     bringup screen (will be replaced as real
                                screens land in later steps)
```

Components for `storage/`, `model/`, `presets/`, `grinder/` are
intentionally NOT scaffolded yet — they'll be added in their respective
build-order steps so the tree only contains live code.
